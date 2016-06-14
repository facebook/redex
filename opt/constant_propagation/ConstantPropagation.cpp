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
#include <vector>

namespace {

  class ConstantPropagation {
  private:
    const Scope& m_scope;
    // The index of the reg_values is the index of registers
    std::vector<AbstractRegister> reg_values;
    // dead_instructions contains contant insns that can be removed after propagation
    std::vector<DexInstruction*> dead_instructions;
    // can_remove maps an instruction to a value indicating whether it can be removed
    std::unordered_map<DexInstruction*, bool> can_remove;
    // method_returns keeps track of constant returned by methods
    std::unordered_map<DexMethod*, int64_t> method_returns;
    size_t m_constant_removed{0};
    size_t m_branch_propagated{0};
    size_t m_method_return_propagated{0};

    void propagate(DexMethod* method) {
      TRACE(CONSTP, 5, "%s\n", show(method).c_str());
      DexInstruction *last_inst = nullptr;
      for (auto const inst : method->get_code()->get_instructions()) {
        TRACE(CONSTP, 2, "instruction: %s\n",  SHOW(inst));
        //If an instruction's type is CONST, it's loaded to specific register and
        //this instruction is marked as can_move until prior instructions change the value of can_move to false
        if (is_const(inst->opcode())) {
          // Only deal with const/4 for simplicity, More constant instruction types will be added later
          if (inst->opcode() == OPCODE_CONST_4 || inst->opcode() == OPCODE_CONST_16) {
            if (inst->dests_size() && inst->has_literal()){
              check_destination(inst);
              auto dest_reg = inst->dest();
              reg_values[dest_reg].known = true;
              reg_values[dest_reg].insn = inst;
              reg_values[dest_reg].val = inst->literal();
              can_remove[inst] = true;
              TRACE(CONSTP, 5, "Move Constant: %d into register: %d\n", inst->literal(), dest_reg);
              m_constant_removed++;
            }
          }
        } else {
          if (inst->srcs_size() > 0) {
            for (unsigned i = 0; i < inst->srcs_size(); i++) {
              if (reg_values[inst->src(i)].known) {
                can_remove[reg_values[inst->src(i)].insn] = false;
              }
            }
          }
          switch (inst->opcode()) {
            case OPCODE_IF_EQZ:
              propagate_branch(inst->opcode(), inst);
              break;
            // If returning a constant value from a known register
            // Save the return value for future use
            case OPCODE_RETURN:
              if (inst->srcs_size() > 0) {
                if (reg_values[inst->src(0)].known) {
                  method_returns[method] = reg_values[inst->src(0)].val;
                }
              }
              break;
            // When there's a move-result instruction following a static-invoke insn
            // Check if there is a constatn returned. If so, propagate the value
            case OPCODE_MOVE_RESULT:
              if (last_inst != nullptr &&
                  last_inst->opcode() == OPCODE_INVOKE_STATIC &&
                  last_inst->has_methods()) {
                  DexOpcodeMethod *referred_method = (DexOpcodeMethod *)last_inst;
                  if (method_returns.find(referred_method->get_method()) != method_returns.end()) {
                    auto return_val = method_returns[referred_method->get_method()];
                    TRACE(CONSTP, 2, "invoke-static instruction: %s\n",  SHOW(last_inst));
                    TRACE(CONSTP, 2, "Find method %s return value: %d\n", SHOW(referred_method), return_val);
                    m_method_return_propagated++;
                    if (inst->size() == 2) {
                      inst->set_opcode(OPCODE_CONST_16);
                    }
                    if (inst->size() == 1) {
                      inst->set_opcode(OPCODE_CONST_4);
                    }
                    inst->set_literal(return_val);
                    TRACE(CONSTP, 2, "Convert move-result to: %s\n", SHOW(inst));
                    referred_method->set_opcode(OPCODE_CONST_16);
                    referred_method->set_dest(inst->dest());
                    referred_method->set_literal(inst->literal());
                  }
              }
            default:
              break;
          }
          if (inst->dests_size()) {
            check_destination(inst);
          }
        }
        last_inst = inst;
      }
      remove_constants(method);
      dead_instructions.clear();
      reg_values.clear();
      can_remove.clear();
    }

    // If a branch reads the value from a register loaded in earlier step,
    // the branch will read value from register, do the evaluation and
    // change the conditional branch to a goto branch if possible
    void propagate_branch(DexOpcode opcode, DexInstruction *inst) {
      if (opcode == OPCODE_IF_EQZ) {
        if (inst->srcs_size() == 1) {
          auto src_reg = inst->src(0);
          if (reg_values[src_reg].known && reg_values[src_reg].val == 0) {
            inst->set_opcode(OPCODE_GOTO_16);
            TRACE(CONSTP, 2, "Changed conditional branch to GOTO Branch offset: %d register %d has value 0\n", inst->offset(), src_reg);
            m_branch_propagated++;
            can_remove[reg_values[src_reg].insn] = true;
          }
        }
      }
    }

    // This function reads the vector of reg_values and check if there is any instruction
    // still marked as can_remove. Then, the program removes all instructions that can be removed
    void remove_constants(DexMethod* method) {
      for (auto& r: reg_values) {
        r.known = false;
        r.insn = nullptr;
      }
    }

    // If there's instruction that overwrites value in a dest registers_size loaded
    // by an earlier instruction. The earier instruction is removed if it has true in can_move
    void check_destination(DexInstruction* inst) {
      auto &dest_reg_value = reg_values[inst->dest()];
      if (dest_reg_value.known /* && can_remove[dest_reg_value.insn] */) {
        dest_reg_value.known = false;
      }
    }

  public:
    ConstantPropagation(const Scope& scope) : m_scope(scope) {
      for (int i = 0; i<REGSIZE; i++) {
        AbstractRegister r = {.known = false, .insn = nullptr, .val = 0};
        reg_values.push_back(AbstractRegister(r));
      }
    }

    void run() {
      TRACE(CONSTP, 1, "Running ConstantPropagation pass\n");
      walk_methods(m_scope,
        [&](DexMethod* m) {
          if (!m->get_code()) {
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

void ConstantPropagationPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
  auto scope = build_class_scope(dexen);
  ConstantPropagation(scope).run();
}
