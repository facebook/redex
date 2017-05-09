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

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

  constexpr const char* METRIC_BRANCH_PROPAGATED =
    "num_branch_propagated";
  constexpr const char* METRIC_METHOD_RETURN_PROPAGATED =
    "num_method_return_propagated";

  // The struct AbstractRegister contains a bool value of whether the value of
  // register is known and the constant value of this register
  struct AbstractRegister {
    bool known;
    int32_t val;
  };

  class ConstantPropagation {
  private:
    const Scope& m_scope;
    const ConstantPropagationPass::Config& m_config;
    // The index of the reg_values is the index of registers
    std::vector<AbstractRegister> reg_values;
    // method_returns keeps track of constant returned by methods
    std::unordered_map<DexMethod*, int32_t> method_returns;
    // Store dead instructions to be removed
    std::vector<IRInstruction*> dead_instructions;
    // Store pairs of intructions to be replaced
    std::vector<std::pair<IRInstruction*, IRInstruction*>> replacements;
    std::vector<std::pair<IRInstruction*, IRInstruction*>>
        branch_replacements;
    size_t m_branch_propagated{0};
    size_t m_method_return_propagated{0};

    void propagate(DexMethod* method) {
      reg_values.clear();
      for (int i = 0; i < method->get_code()->get_registers_size(); i++) {
        AbstractRegister r = {.known = false, .val = 0};
        reg_values.push_back(AbstractRegister(r));
      }

      bool changed = true;
      TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
      TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));
      // The loop traverses all blocks in the method until no more branch is converted
      while (changed) {
        auto code = method->get_code();
        code->build_cfg();
        auto& cfg = code->cfg();
        TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
        auto first_block = cfg.blocks()[0];
        // block_preds saves the re-calculated number of predecessors of each block
        // This number may change as new unreachable blocks are added in each loop
        std::unordered_map<Block*, int> block_to_predecessors_count;
        find_reachable_predecessors(cfg.blocks(), block_to_predecessors_count);
        changed = propagate_constant_in_method(
            method, first_block, block_to_predecessors_count);
        apply_changes(code);
      }
    }

    bool propagate_constant_in_method(
        DexMethod* method,
        Block* first_block,
        std::unordered_map<Block*, int>& block_preds) {
      bool changed = false;
      std::stack<Block*> dfs_front;
      dfs_front.push(first_block);
      std::unordered_map<Block*, bool> visited;
      visited[first_block] = true;
      // This loop traverses each block by depth-first
      while (!dfs_front.empty() && !changed) {
        auto current_block = dfs_front.top();
        dfs_front.pop();
        TRACE(CONSTP, 5, "Processing block %d\n", current_block->id());
        if (block_preds[current_block] != 1) {
          TRACE(CONSTP, 5, "More than one pred, removing constants\n");
          remove_constants();
        }
        IRInstruction* last_inst = nullptr;
        for (auto it = current_block->begin();
             it != current_block->end() && !changed;
             ++it) {
          if (it->type != MFLOW_OPCODE) {
            continue;
          }
          auto inst = it->insn;
          TRACE(CONSTP, 5, "instruction: %s\n", SHOW(inst));
          if (is_const(inst->opcode())) {
            propagate_constant(inst);
          } else {
            changed = propagate_insn(inst, last_inst, method);
          }
        }
        // Propagate successive blocks
        int succ_num = 0;
        for (auto successor_block : current_block->succs()) {
          if (!visited[successor_block]) {
            dfs_front.push(successor_block);
            visited[successor_block] = true;
            succ_num++;
          }
        }
        if (succ_num != 1) {
          TRACE(CONSTP, 5, "More than one successor, removing constants\n");
          remove_constants();
        }
      }
      return changed;
    }

    void apply_changes(IRCode* code) {
      for (auto const& p : replacements) {
        auto const& old_op = p.first;
        auto const& new_op = p.second;
        code->replace_opcode(old_op, new_op);
      }
      replacements.clear();

      for (auto const& p : branch_replacements) {
        auto const& old_op = p.first;
        auto const& new_op = p.second;
        if (new_op->opcode() == OPCODE_NOP) {
          code->remove_opcode(old_op);
          delete new_op;
        } else {
          code->replace_branch(old_op, new_op);
        }
      }
      branch_replacements.clear();

      for (auto dead : dead_instructions) {
        code->remove_opcode(dead);
      }
      dead_instructions.clear();
    }

    bool propagate_insn(IRInstruction *inst, IRInstruction *&last_inst, DexMethod *method) {
      bool changed = false;
      switch (inst->opcode()) {
      case OPCODE_IF_EQ:
      case OPCODE_IF_NE:
      case OPCODE_IF_LT:
      case OPCODE_IF_GE:
      case OPCODE_IF_GT:
      case OPCODE_IF_LE:
      case OPCODE_IF_LTZ:
      case OPCODE_IF_GEZ:
      case OPCODE_IF_GTZ:
      case OPCODE_IF_LEZ:
        if (m_config.only_simple_branches) {
          break;
        }
        // fallthrough
      case OPCODE_IF_EQZ:
      case OPCODE_IF_NEZ: {
        IRInstruction* branch_replacement = propagate_branch(inst);
        if (branch_replacement != nullptr) {
          TRACE(CONSTP, 2, "Changed conditional branch %s\n", SHOW(inst));
          branch_replacements.emplace_back(inst, branch_replacement);
          m_branch_propagated++;
          changed = true;
        }
        break;
      }

      // For return instruction, save the return value for future use
      case OPCODE_RETURN:
        if (reg_values[inst->src(0)].known)
          method_returns[method] = reg_values[inst->src(0)].val;
        break;
      // For move-result instruction following a static-invoke insn
      // Check if there is a constant returned. If so, propagate the value
      case OPCODE_MOVE_RESULT:
        reg_values[inst->dest()].known = false;
        if (false && // deactivate return propagation for now
            last_inst != nullptr &&
            last_inst->opcode() == OPCODE_INVOKE_STATIC &&
            last_inst->has_method()) {
          if (method_returns.find(last_inst->get_method()) !=
              method_returns.end()) {
            auto return_val = method_returns[last_inst->get_method()];
            TRACE(CONSTP,
                  2,
                  "Find method %s return value: %d\n",
                  SHOW(last_inst),
                  return_val);
            m_method_return_propagated++;
            auto new_inst = (new IRInstruction(OPCODE_CONST_16))
                                ->set_dest(inst->dest())
                                ->set_literal(return_val);
            replacements.emplace_back(inst, new_inst);
            dead_instructions.push_back(last_inst);
            propagate_constant(new_inst);
          }
        }
        break;
      // For move instruction, propagate known status and the value to the dest
      // register
      case OPCODE_MOVE:
      case OPCODE_MOVE_OBJECT:
        reg_values[inst->dest()].known = reg_values[inst->src(0)].known;
        reg_values[inst->dest()].val = reg_values[inst->src(0)].val;
        break;
      default:
        if (inst->dests_size()) {
          reg_values[inst->dest()].known = false;
          if (inst->dest_is_wide()) {
            reg_values[inst->dest() + 1].known = false;
          }
        }
        break;
      }
      last_inst = inst;
      return changed;
    }

    // Propagate const instruction value to registers
    void propagate_constant(IRInstruction *inst) {
      // Only deal with const/4 and OPCODE_CONST_16 for simplicity
      if (inst->opcode() == OPCODE_CONST_4 ||
          inst->opcode() == OPCODE_CONST_16) {
          reg_values[inst->dest()].known = true;
          reg_values[inst->dest()].val = inst->literal();
      } else {
        reg_values[inst->dest()].known = false;
        if (inst->dest_is_wide()) {
          reg_values[inst->dest() + 1].known = false;
        }
      }
    }

    // Evaluate the guard expression of an if opcode.
    // pass 0 as the r_val for if-*Z opcodes
    bool eval_if(DexOpcode op, int32_t l_val, int32_t r_val) {
      switch (op) {
      case OPCODE_IF_EQ:
      case OPCODE_IF_EQZ:
        return l_val == r_val;
      case OPCODE_IF_NE:
      case OPCODE_IF_NEZ:
        return l_val != r_val;
      case OPCODE_IF_LT:
      case OPCODE_IF_LTZ:
        return l_val < r_val;
      case OPCODE_IF_GE:
      case OPCODE_IF_GEZ:
        return l_val >= r_val;
      case OPCODE_IF_GT:
      case OPCODE_IF_GTZ:
        return l_val > r_val;
      case OPCODE_IF_LE:
      case OPCODE_IF_LEZ:
        return l_val <= r_val;
      default:
        always_assert_log(false, "opcode %s must be an if", SHOW(op));
      }
    }

    // Attempt to create replacements for branch instructions. Evaluate the
    // conditional based on known register values.
    //
    // returns a new GOTO_16 if this branch is always true.
    // returns a new NOP if this branch is always false. Be sure to free it.
    // returns nullptr when this branch can't be removed
    //
    // precondition: inst must be a branch instruction (OPCODE_IF_*)
    IRInstruction* propagate_branch(IRInstruction *inst) {
      auto left = inst->src(0);
      if (!reg_values[left].known) {
        return nullptr;
      }

      int32_t l_val = reg_values[left].val;
      int32_t r_val;
      if (inst->srcs_size() == 2) {
        AbstractRegister right = reg_values[inst->src(1)];
        if (right.known) {
          // if vA, vB with both values known
          r_val = right.val;
        } else {
          // if vA, vB where vB isn't known
          return nullptr;
        }
      } else {
        // if-*Z vA        is the same as
        // if-*  vA, 0
        r_val = 0;
      }

      bool branch_result = eval_if(inst->opcode(), l_val, r_val);
      if (branch_result) {
        return new IRInstruction(OPCODE_GOTO_16);
      } else if (!m_config.only_simple_branches) {
        return new IRInstruction(OPCODE_NOP);
      }
      return nullptr;
    }

    // This function reads the vector of reg_values and marks all regs to unknown
    void remove_constants() {
      for (auto &r: reg_values) {
        r.known = false;
      }
    }

    // This method calculates number of reachable predecessors of each block
    // The difference between this method and remove_unreachable_blocks in
    // LocalDCE is that
    // this method only finds unreachable blocks without deleting any edge
    // or block
    void find_reachable_predecessors(
        const std::vector<Block*>& original_blocks,
        std::unordered_map<Block*, int>& block_to_predecessors_count) {
      std::vector<Block*> unreachable_blocks;
      for (size_t i = 1; i < original_blocks.size(); ++i) {
        auto pred_size = original_blocks[i]->preds().size();
        block_to_predecessors_count[original_blocks[i]] = pred_size;
        if (pred_size == 0) {
          unreachable_blocks.push_back(original_blocks[i]);
        }
      }
      while (unreachable_blocks.size() > 0) {
        auto b = unreachable_blocks.back();
        unreachable_blocks.pop_back();
        for (auto succs_b : b->succs()) {
          if (--block_to_predecessors_count[succs_b] == 0) {
            unreachable_blocks.push_back(succs_b);
          }
        }
      }
    }

  public:
    ConstantPropagation(
        const Scope& scope,
        const ConstantPropagationPass::Config& cfg)
      : m_scope(scope), m_config(cfg) {}

    void run() {
      TRACE(CONSTP, 1, "Running ConstantPropagation pass\n");
      walk_methods(m_scope,
        [&](DexMethod* m) {
          if (!m->get_code()) {
            return;
          }
          // Skipping blacklisted classes
          if (m_config.blacklist.count(m->get_class()) > 0) {
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

    size_t num_branch_propagated() const {
      return m_branch_propagated;
    }

    size_t num_method_return_propagated() const {
      return m_method_return_propagated;
    }
  };
}

void ConstantPropagationPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  ConstantPropagation constant_prop(scope, m_config);
  constant_prop.run();
  mgr.incr_metric(
    METRIC_BRANCH_PROPAGATED,
    constant_prop.num_branch_propagated());
  mgr.incr_metric(
    METRIC_METHOD_RETURN_PROPAGATED,
    constant_prop.num_method_return_propagated());
}

static ConstantPropagationPass s_pass;
