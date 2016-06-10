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
    size_t m_constant_removed{0};
    size_t m_branch_propagated{0};

    void propagate(DexMethod* method) {
      TRACE(CONSTP, 2, "%s\n", show(method).c_str());
      for (auto const inst : method->get_code()->get_instructions()) {
        TRACE(CONSTP, 2, "instruction: %s\n",  SHOW(inst));
        //If an instruction's type is CONST, it's loaded to specific register and
        //this instruction is marked as can_move until prior instructions change the value of can_move to false
        if (is_const(inst->opcode())) {
          // Only deal with const/4 for simplicity, More constant instruction types will be added later
          if (inst->opcode() == OPCODE_CONST_4) {
            if (inst->dests_size() && inst->has_literal()){
              check_destination(inst);
              auto dest_reg = inst->dest();
              reg_values[dest_reg].known = true;
              reg_values[dest_reg].insn = inst;
              reg_values[dest_reg].val = inst->literal();
              can_remove[inst] = true;
              TRACE(CONSTP, 2, "Move Constant: %d into register: %d\n", inst->literal(), dest_reg);
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
          if (is_branch(inst->opcode())){
            propagate_branch(inst->opcode(), inst);
          }
          if (inst->dests_size()) {
            check_destination(inst);
          }
        }
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
      switch (opcode) {
        case OPCODE_IF_EQZ:
          if (inst->srcs_size() == 1) {
            auto src_reg = inst->src(0);
            if (reg_values[src_reg].known && reg_values[src_reg].val == 0) {
              inst->set_opcode(OPCODE_GOTO_16);
              TRACE(CONSTP, 2, "Changed conditional branch to GOTO Branch offset: %d \n", inst->offset());
              m_branch_propagated++;
              can_remove[reg_values[src_reg].insn] = true;
            }
          }
          break;
        default:
          break;
      }
    }

    // This function reads the vector of reg_values and check if there is any instruction
    // still marked as can_remove. Then, the program removes all instructions that can be removed
    void remove_constants(DexMethod* method) {
      for (auto& r: reg_values) {
        if (r.known && can_remove[r.insn]) {
          dead_instructions.push_back(r.insn);
        }
        r.known = false;
        r.insn = nullptr;
      }
      m_constant_removed += dead_instructions.size();
    }

    // If there's instruction that overwrites value in a dest registers_size loaded
    // by an earlier instruction. The earier instruction is removed if it has true in can_move
    void check_destination(DexInstruction* inst) {
      auto dest_reg_value = reg_values[inst->dest()];
      if (dest_reg_value.known && can_remove[dest_reg_value.insn]) {
        dead_instructions.push_back(dest_reg_value.insn);
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
          "Constant removed: %lu\n",
          m_constant_removed);
          TRACE(CONSTP, 1,
            "Branch condition propagated: %lu\n",
            m_branch_propagated);
          }
        };

      }

      ////////////////////////////////////////////////////////////////////////////////

      void ConstantPropagationPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
        auto scope = build_class_scope(dexen);
        ConstantPropagation(scope).run();
      }
