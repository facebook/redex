/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "ConstantPropagation.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Transform.h"
#include "walkers.h"
#include "LocalDce.h"
#include <vector>
#include <stack>

namespace {

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
    size_t m_constant_removed{0};
    size_t m_branch_propagated{0};
    size_t m_method_return_propagated{0};

    void propagate(DexMethod* method) {
      bool changed = true;
      TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
      TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));
      // The loop traverses all blocks in the method until no more branch is converted
      while (changed) {
        if (!method->get_code()) {
          return;
        }
        changed = false;
        auto transform = MethodTransform::get_method_transform(method, true);
        auto& cfg = transform->cfg();
        std::stack<Block *> blocks;
        blocks.push(cfg[0]);
        std::unordered_map<Block *, bool> visited;
        for (const auto b: cfg) {
          visited[b] = false;
        }
        // This loop traverses each block by depth-first
        while (!blocks.empty()) {
          auto b = blocks.top();
          blocks.pop();
          visited[b] = true;
          DexInstruction *last_inst = nullptr;
          for (auto it = b->begin(); it != b->end() && !changed; ++it) {
            if (it->type != MFLOW_OPCODE) {
              continue;
            }
            auto inst = it->insn;
            TRACE(CONSTP, 5, "instruction: %s\n",  SHOW(inst));
            // If an instruction's type is CONST, it's loaded to a specific register
            // Then the register is marked as known.
            if (is_const(inst->opcode())) {
              propagate_constant(inst);
            } else {
              changed = propagate_insn(inst, last_inst, method);
            }
          }
          if (changed) {
            remove_constants(method);
            break;
          }
          // Only propagate to next block when it's the only successive block
          if (b->succs().size() == 1) {
            for (auto succs_b: b->succs()) {
              if (!visited[succs_b])
              blocks.push(succs_b);
            }
          } else {
            remove_constants(method);
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
        remove_constants(method);
        transform->sync();
      }
    }

    bool propagate_insn(DexInstruction *inst, DexInstruction *&last_inst, DexMethod *method) {
      bool changed = false;
      if (inst->dests_size()) {
        reg_values[inst->dest()].known = false;
      }
      switch (inst->opcode()) {
        case OPCODE_IF_NEZ:
        case OPCODE_IF_EQZ:
          if (propagate_branch(inst)) {
            TRACE(CONSTP, 2, "Class: %s\n",  SHOW(method->get_class()));
            TRACE(CONSTP, 2, "Method: %s\n %s\n",  SHOW(method->get_name()), SHOW(method->get_code()));
            changed = true;
          }
          break;
        // If returning a constant value from a known register
        // Save the return value for future use
        case OPCODE_RETURN:
          if (reg_values[inst->src(0)].known)
            method_returns[method] = reg_values[inst->src(0)].val;
          break;
        // When there's a move-result instruction following a static-invoke insn
        // Check if there is a constant returned. If so, propagate the value
        case OPCODE_MOVE_RESULT:
          if (last_inst != nullptr &&
              (last_inst->opcode() == OPCODE_INVOKE_STATIC ||
              last_inst->opcode() == OPCODE_INVOKE_VIRTUAL) &&
              last_inst->has_methods()) {
              DexOpcodeMethod *referred_method = static_cast<DexOpcodeMethod *>(last_inst);
              if (method_returns.find(referred_method->get_method()) != method_returns.end()) {
                auto return_val = method_returns[referred_method->get_method()];
                TRACE(CONSTP, 2, "invoke-static instruction: %s\n",  SHOW(last_inst));
                TRACE(CONSTP, 2, "Find method %s return value: %d\n", SHOW(referred_method), return_val);
                TRACE(CONSTP, 5, "Class: %s\n",  SHOW(method->get_class()));
                m_method_return_propagated++;
                auto new_inst = (new DexInstruction(OPCODE_CONST_16))->set_dest(inst->dest())->set_literal(return_val);
                replaces.emplace_back(inst, new_inst);
                dead_instructions.push_back(referred_method);
                TRACE(CONSTP, 5, "Tranformed Method: %s\n%s\n",  SHOW(method->get_name()), SHOW(method->get_code()));
                propagate_constant(inst);
                changed = true;
              }
          }
          break;
          // If there is a move_object instruction and the src reg is known
          // Propagate the value to the dest register
        case OPCODE_MOVE:
        case OPCODE_MOVE_OBJECT:
          if (reg_values[inst->src(0)].known) {
            auto &src_reg_value = reg_values[inst->src(0)];
            reg_values[inst->dest()].known = true;
            reg_values[inst->dest()].val = src_reg_value.val;
          }
          break;
        default:
          break;
      }
      last_inst = inst;
      return changed;
    }

    // Propagate const instruction value to registers
    void propagate_constant(DexInstruction *inst) {
      // Only deal with const/4 and OPCODE_CONST_16 for simplicity, More constant instruction types will be added later
      if (inst->opcode() == OPCODE_CONST_4 ||
          inst->opcode() == OPCODE_CONST_16) {
        if (inst->dests_size() && inst->has_literal()){
          auto dest_reg = inst->dest();
          reg_values[dest_reg].known = true;
          reg_values[dest_reg].val = inst->literal();
          TRACE(CONSTP, 5, "Move Constant: %d into register: %d\n", inst->literal(), dest_reg);
          m_constant_removed++;
        }
      }
    }

    // If a branch reads the value from a register loaded in earlier step,
    // the branch will read value from register, do the evaluation and
    // change the conditional branch to a goto branch if possible
    bool propagate_branch(DexInstruction *inst) {
      if (inst->srcs_size() == 1) {
        if (inst->opcode() == OPCODE_IF_EQZ) {
          auto src_reg = inst->src(0);
          if (reg_values[src_reg].known && reg_values[src_reg].val == 0) {
            TRACE(CONSTP, 2, "Changed conditional branch %s ", SHOW(inst));
            auto new_inst = (new DexInstruction(OPCODE_GOTO_16))->set_offset(inst->offset());
            replaces.emplace_back(inst, new_inst);
            TRACE(CONSTP, 2, "to GOTO Branch offset: %d register %d has value 0\n", inst->offset(), src_reg);
            m_branch_propagated++;
            return true;
          }
        }
        if (inst->opcode() == OPCODE_IF_NEZ) {
          auto src_reg = inst->src(0);
          if (reg_values[src_reg].known && reg_values[src_reg].val != 0) {
            TRACE(CONSTP, 2, "Changed conditional branch %s ", SHOW(inst));
            auto new_inst = (new DexInstruction(OPCODE_GOTO_16))->set_offset(inst->offset());
            replaces.emplace_back(inst, new_inst);
            TRACE(CONSTP, 2, "to GOTO Branch offset: %d register %d has value 0\n", inst->offset(), src_reg);
            m_branch_propagated++;
            return true;
          }
        }
      }
      return false;
    }

    // This function reads the vector of reg_values and marks all regs to unknown
    void remove_constants(DexMethod* method) {
      for (auto &r: reg_values) {
        r.known = false;
        r.insn = nullptr;
      }
    }

  public:
    ConstantPropagation(const Scope& scope) : m_scope(scope) {
      for (int i = 0; i<REGSIZE; i++) {
        AbstractRegister r = {.known = false, .insn = nullptr, .val = 0};
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
        "Constant propagated: %lu\n",
        m_constant_removed);
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

void ConstantPropagationPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
  auto scope = build_class_scope(dexen);
  auto pre_opt_classes = LocalDcePass::find_referenced_classes(scope);
  auto blacklist_classes = get_black_list(m_blacklist);
  ConstantPropagation(scope).run(blacklist_classes);
  MethodTransform::sync_all();
  auto post_opt_classes = LocalDcePass::find_referenced_classes(scope);
  scope.erase(remove_if(scope.begin(), scope.end(),
    [&](DexClass* cls) {
      if (pre_opt_classes.find(cls) != pre_opt_classes.end() &&
      post_opt_classes.find(cls) == pre_opt_classes.end()) {
        TRACE(DCE, 2, "Removed class: %s\n", cls->get_name()->c_str());
        return true; }
      return false; }),
    scope.end());
  post_dexen_changes(scope, dexen);
}
