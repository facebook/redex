/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "ConstantPropagation.h"

#include <vector>
#include <stack>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

  constexpr int REGSIZE = 256;

  // The struct AbstractRegister contains a bool value of whether the value of
  // register is known and the constant value of this register
  struct AbstractRegister {
    bool known;
    int64_t val;
  };

  class ConstantPropagation {
  private:
    const Scope& m_scope;
    // The index of the reg_values is the index of registers
    std::vector<AbstractRegister> reg_values;
    // method_returns keeps track of constant returned by methods
    std::unordered_map<DexMethod*, int64_t> method_returns;
    // Store dead instructions to be removed
    std::vector<DexInstruction*> dead_instructions;
    // Store pairs of intructions to be replaced
    std::vector<std::pair<DexInstruction*, DexInstruction*>> replaces;
    size_t m_branch_propagated{0};
    size_t m_method_return_propagated{0};

    void propagate(DexMethod* method) {
      bool changed = true;
      TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
      TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));
      // The loop traverses all blocks in the method until no more branch is converted
      while (changed) {
        changed = false;
        auto transform = MethodTransform::get_method_transform(method, true);
        auto& cfg = transform->cfg();
        std::unordered_map<Block *, bool> visited;
        std::stack<Block *> blocks;
        blocks.push(cfg[0]);
        visited[cfg[0]] = true;
        // block_preds saves the re-calculated number of predecessors of each block
        // This number may change as new unreachable blocks are added in each loop
        std::unordered_map<Block*, int> block_preds;
        find_reachable_predecessors(cfg, block_preds);
        // This loop traverses each block by depth-first
        while (!blocks.empty() && !changed) {
          auto b = blocks.top();
          blocks.pop();
          if (block_preds[b] != 1) {
            remove_constants();
          }
          DexInstruction *last_inst = nullptr;
          for (auto it = b->begin(); it != b->end() && !changed; ++it) {
            if (it->type != MFLOW_OPCODE) {
              continue;
            }
            auto inst = it->insn;
            TRACE(CONSTP, 5, "instruction: %s\n",  SHOW(inst));
            if (is_const(inst->opcode())) {
              propagate_constant(inst);
            } else {
              changed = propagate_insn(inst, last_inst, method);
            }
          }
          // Propagate successive blocks
          int succ_num = 0;
          for (auto succs_b: b->succs()) {
            if (!visited[succs_b]) {
              blocks.push(succs_b);
              visited[succs_b] = true;
              succ_num++;
            }
          }
          if (succ_num != 1) {
            remove_constants();
          }
        }
        for (auto const& p : replaces) {
          auto const& old_op = p.first;
          auto const& new_op = p.second;
          transform->replace_opcode(old_op, new_op);
        }
        replaces.clear();
        for (auto dead: dead_instructions) {
          transform->remove_opcode(dead);
        }
        dead_instructions.clear();
        transform->sync();
      }
    }

    bool propagate_insn(DexInstruction *inst, DexInstruction *&last_inst, DexMethod *method) {
      bool changed = false;
      switch (inst->opcode()) {
        case OPCODE_IF_NEZ:
        case OPCODE_IF_EQZ:
          if (propagate_branch(inst)) {
            TRACE(CONSTP, 2, "Changed conditional branch %s\n", SHOW(inst));
            auto new_inst = (new DexInstruction(OPCODE_GOTO_16))->set_offset(inst->offset());
            replaces.emplace_back(inst, new_inst);
            m_branch_propagated++;
            changed = true;
          }
          break;
        // For return instruction, save the return value for future use
        case OPCODE_RETURN:
          if (reg_values[inst->src(0)].known)
            method_returns[method] = reg_values[inst->src(0)].val;
          break;
        // For move-result instruction following a static-invoke insn
        // Check if there is a constant returned. If so, propagate the value
        case OPCODE_MOVE_RESULT:
          reg_values[inst->dest()].known = false;
          if (last_inst != nullptr &&
              last_inst->opcode() == OPCODE_INVOKE_STATIC &&
              last_inst->has_methods()) {
              DexOpcodeMethod *referred_method = static_cast<DexOpcodeMethod *>(last_inst);
              if (method_returns.find(referred_method->get_method()) != method_returns.end()) {
                auto return_val = method_returns[referred_method->get_method()];
                TRACE(CONSTP, 2, "Find method %s return value: %d\n", SHOW(referred_method), return_val);
                m_method_return_propagated++;
                auto new_inst = (new DexInstruction(OPCODE_CONST_16))->set_dest(inst->dest())->set_literal(return_val);
                replaces.emplace_back(inst, new_inst);
                dead_instructions.push_back(referred_method);
                propagate_constant(new_inst);
              }
          }
          break;
        // For move instruction, propagate known status and the value to the dest register
        case OPCODE_MOVE:
        case OPCODE_MOVE_OBJECT:
          reg_values[inst->dest()].known = reg_values[inst->src(0)].known;
          reg_values[inst->dest()].val = reg_values[inst->src(0)].val;
          break;
        default:
          if (inst->dests_size()) {
            reg_values[inst->dest()].known = false;
          }
          break;
      }
      last_inst = inst;
      return changed;
    }

    // Propagate const instruction value to registers
    void propagate_constant(DexInstruction *inst) {
      // Only deal with const/4 and OPCODE_CONST_16 for simplicity
      if (inst->opcode() == OPCODE_CONST_4 ||
          inst->opcode() == OPCODE_CONST_16) {
          reg_values[inst->dest()].known = true;
          reg_values[inst->dest()].val = inst->literal();
      } else {
        reg_values[inst->dest()].known = false;
      }
    }

    // If a branch reads the value from a register loaded in earlier step,
    // the branch will read value from register, do the evaluation and
    // change the conditional branch to a goto branch if possible
    bool propagate_branch(DexInstruction *inst) {
      auto src_reg = inst->src(0);
      if (reg_values[src_reg].known) {
        if (inst->opcode() == OPCODE_IF_EQZ && reg_values[src_reg].val == 0) {
            return true;
        }
        if (inst->opcode() == OPCODE_IF_NEZ && reg_values[src_reg].val != 0) {
            return true;
        }
      }
      return false;
    }

    // This function reads the vector of reg_values and marks all regs to unknown
    void remove_constants() {
      for (auto &r: reg_values) {
        r.known = false;
      }
    }

    // This method calculates number of reachable predecessors of each block
    // The difference between this method and remove_unreachable_blocks in LocalDCE is that
    // this method only finds unreachable classes without deleting any edge or block
    void find_reachable_predecessors(std::vector<Block*>& blocks,
                                     std::unordered_map<Block*, int>& block_preds) {
      std::vector<Block*> unreachable_blocks;
      for (size_t i = 1; i < blocks.size(); ++i) {
        auto pred_size = blocks[i]->preds().size();
        block_preds[blocks[i]] = pred_size;
        if (pred_size == 0) {
          unreachable_blocks.push_back(blocks[i]);
        }
      }
      while (unreachable_blocks.size() > 0) {
        auto b = unreachable_blocks.back();
        unreachable_blocks.pop_back();
        for (auto succs_b: b->succs()) {
          if (--block_preds[succs_b] == 0) {
            unreachable_blocks.push_back(succs_b);
          }
        }
      }
    }

  public:
    ConstantPropagation(const Scope& scope) : m_scope(scope) {
      for (int i = 0; i<REGSIZE; i++) {
        AbstractRegister r = {.known = false, .val = 0};
        reg_values.push_back(AbstractRegister(r));
      }
    }

    void run(const std::unordered_set<DexType*> &blacklist_classes) {
      TRACE(CONSTP, 1, "Running ConstantPropagation pass\n");
      walk_methods(m_scope,
        [&](DexMethod* m) {
          if (!m->get_code()) {
            return;
          }
          // Skipping blacklisted classes
          if (blacklist_classes.count(m->get_class()) > 0) {
            TRACE(CONSTP, 2, "Skipping %s\n", show(m->get_class()).c_str());
            return;
          }
          propagate(m);
        });

      TRACE(CONSTP, 1,
        "Branch condition removed: %lu\n",
        m_branch_propagated);
      TRACE(CONSTP, 1,
        "Static function invocation removed: %lu\n",
        m_method_return_propagated);
    }
  };
}

////////////////////////////////////////////////////////////////////////////////

std::unordered_set<DexType*> get_black_list(
  const std::vector<std::string>& config
) {
  std::unordered_set<DexType*> blacklist;
  for (auto const& config_blacklist : config) {
    DexType* entry = DexType::get_type(config_blacklist.c_str());
    if (entry) {
      TRACE(CONSTP, 2, "blacklist class: %s\n", SHOW(entry));
      blacklist.insert(entry);
    }
  }
  return blacklist;
}

void ConstantPropagationPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto blacklist_classes = get_black_list(m_blacklist);
  ConstantPropagation(scope).run(blacklist_classes);
}

static ConstantPropagationPass s_pass;
