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

namespace {

  static bool has_side_effects(DexOpcode opc) {
    switch (opc) {
      case OPCODE_RETURN_VOID:
      case OPCODE_RETURN:
      case OPCODE_RETURN_WIDE:
      case OPCODE_RETURN_OBJECT:
      case OPCODE_MONITOR_ENTER:
      case OPCODE_MONITOR_EXIT:
      case OPCODE_CHECK_CAST:
      case OPCODE_FILL_ARRAY_DATA:
      case OPCODE_THROW:
      case OPCODE_GOTO:
      case OPCODE_GOTO_16:
      case OPCODE_GOTO_32:
      case OPCODE_PACKED_SWITCH:
      case OPCODE_SPARSE_SWITCH:
      case OPCODE_APUT:
      case OPCODE_APUT_WIDE:
      case OPCODE_APUT_OBJECT:
      case OPCODE_APUT_BOOLEAN:
      case OPCODE_APUT_BYTE:
      case OPCODE_APUT_CHAR:
      case OPCODE_APUT_SHORT:
      case OPCODE_IPUT:
      case OPCODE_IPUT_WIDE:
      case OPCODE_IPUT_OBJECT:
      case OPCODE_IPUT_BOOLEAN:
      case OPCODE_IPUT_BYTE:
      case OPCODE_IPUT_CHAR:
      case OPCODE_IPUT_SHORT:
      case OPCODE_SPUT:
      case OPCODE_SPUT_WIDE:
      case OPCODE_SPUT_OBJECT:
      case OPCODE_SPUT_BOOLEAN:
      case OPCODE_SPUT_BYTE:
      case OPCODE_SPUT_CHAR:
      case OPCODE_SPUT_SHORT:
      case OPCODE_INVOKE_VIRTUAL:
      case OPCODE_INVOKE_SUPER:
      case OPCODE_INVOKE_DIRECT:
      case OPCODE_INVOKE_STATIC:
      case OPCODE_INVOKE_INTERFACE:
      case OPCODE_INVOKE_VIRTUAL_RANGE:
      case OPCODE_INVOKE_SUPER_RANGE:
      case OPCODE_INVOKE_DIRECT_RANGE:
      case OPCODE_INVOKE_STATIC_RANGE:
      case OPCODE_INVOKE_INTERFACE_RANGE:
      case FOPCODE_PACKED_SWITCH:
      case FOPCODE_SPARSE_SWITCH:
      case FOPCODE_FILLED_ARRAY:
      return true;
      default:
      return false;
    }
    not_reached();
  }

  class ConstantPropagation {
  private:
    const Scope& m_scope;
    //size_t m_constant_variables{0};
    //size_t m_instructions_propagated{0};

    void propagate(DexMethod* method) {
      if (strcmp(method->get_name()->c_str(), "propagation_1") == 0) {
        TRACE(CONSTP, 2, "%s\n", show(method).c_str());
        for (auto const inst : method->get_code()->get_instructions()) {
          TRACE(CONSTP, 2, "instruction: %s\n",  SHOW(inst));
          if (!has_side_effects(inst->opcode())) {
            if (inst->has_literal()) {
              TRACE(CONSTP, 2, "Constant: %d ", inst->literal());
            }
            for (unsigned i = 0; i < inst->srcs_size(); i++) {
              TRACE(CONSTP, 2, "Source register: %d ", inst->src(i));
            }
            if (inst->dests_size()) {
              TRACE(CONSTP, 2, "Dest register: %d\n", inst->dest());
            }
            if (is_branch(inst->opcode())){
              TRACE(CONSTP, 2, "Branch offset: %d\n", inst->offset());
            }
            TRACE(CONSTP, 2, "\n");
          }
        }
      }
    }

  public:
    ConstantPropagation(const Scope& scope) : m_scope(scope) {}

    void run() {
      walk_methods(m_scope,
        [&](DexMethod* m) {
          if (!m->get_code()) {
            return;
          }
          propagate(m);
        });
      }
    };

  }

  ////////////////////////////////////////////////////////////////////////////////

  void ConstantPropagationPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
    auto scope = build_class_scope(dexen);
    ConstantPropagation(scope).run();
  }
