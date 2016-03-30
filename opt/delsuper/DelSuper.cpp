/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DelSuper.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "walkers.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "ReachableClasses.h"


namespace {

static const DexCodeItemOpcode s_return_invoke_super_void_opcs[2] = {
  OPCODE_INVOKE_SUPER, OPCODE_RETURN_VOID
};

static const DexCodeItemOpcode s_return_invoke_super_opcs[3] = {
  OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT, OPCODE_RETURN
};

static const DexCodeItemOpcode s_return_invoke_super_wide_opcs[3] = {
  OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT_WIDE, OPCODE_RETURN_WIDE
};

static const DexCodeItemOpcode s_return_invoke_super_obj_opcs[3] = {
  OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT_OBJECT, OPCODE_RETURN_OBJECT
};

class DelSuper {

private:
  const std::vector<DexClass*>& m_scope;
  std::vector<DexMethod*> m_delmeths;
  int m_num_methods;
  int m_num_passed;
  int m_num_trivial;
  int m_num_relaxed_vis;
  int m_num_private;
  int m_num_culled_no_code;
  int m_num_culled_too_short;
  int m_num_culled_not_trivial;
  int m_num_culled_static;
  int m_num_culled_name_differs;
  int m_num_culled_proto_differs;
  int m_num_culled_return_move_result_differs;
  int m_num_culled_args_differs;
  int m_num_culled_super_is_non_public_sdk;
  int m_num_culled_super_not_def;


  /**
   * This method ensures that the method arguments pass directly through
   * to the super invocation, for methods where the prototypes are already,
   * known to match.
   *
   * matching:
   *
   *   void method(int a1, int a2, int a3) {
   *     super.method(a1, a2, a3);
   *   }
   *
   * NOT matching:
   *
   *   void method(int a1, int a2, int a3) {
   *     super.method(a1, a1, a1);
   *   }
   *
   *   void method(int a1, int a2, int a3) {
   *     super.method(a3, a2, a1);
   *   }
   *
   *   void method(int a1, int a2, int a3) {
   *     super.method(a1, a2, 0);
   *   }
   *
   * Cases where method prototypes don't even match (e.g. different
   * number of types of arguments) are filtered before hand so we
   * don't handle that case here.
   *
   */
  bool do_invoke_meth_args_pass_through(
    const DexMethod* meth,
    const DexOpcode* opc) {
    assert(opc->opcode() == OPCODE_INVOKE_SUPER);
    assert(opc->has_arg_word_count() == true);
    uint16_t start_reg = meth->get_code()->get_registers_size() -
      meth->get_code()->get_ins_size();
    uint16_t end_reg = meth->get_code()->get_registers_size();
    // args to this method start at 'start_reg', so they should
    // be passed into invoke_super of opc {start_reg, ..., end_reg}
    uint16_t arg_word_count = opc->arg_word_count();
    if (arg_word_count != end_reg - start_reg) {
      return false;
    }
    // N.B. don't need to check reg < end_reg in for loop
    // because of above length check
    for (uint16_t i = 0, reg = start_reg ; i < arg_word_count ; ++i, ++reg) {
      uint16_t opc_reg = opc->src(i);
      if (reg != opc_reg) {
        return false;
      }
    }
    return true;
  }

  bool are_opcs_equal(
    const std::vector<DexOpcode*> insns,
    const DexCodeItemOpcode* opcs,
    size_t opcs_len) {
    if (insns.size() != opcs_len) return false;
    for (size_t i = 0 ; i < opcs_len ; ++i) {
      if (insns[i]->opcode() != opcs[i]) return false;
    }
    return true;
  }

  /**
   * Trivial return invoke supers are:
   *
   * - Must have a body (bytecode)
   * - Opcodes must be match one pattern exactly (no more, no less):
   *   - invoke-super, return-void (void)
   *   - invoke-super, move-result, return (prim)
   *   - invoke-super, move-result-wide, return-wide (wide prim)
   *   - invoke-super, move-result-object, return-object (obj)
   * - Not static methods
   * - Method name must match name of super method
   * - Method proto must match name of super method
   * - Method vis must match vis of super method
   * - Method return src register must match move-result dest register
   * - Method args must all go into invoke without rearrangement
   */
  bool is_trivial_return_invoke_super(const DexMethod* meth) {
    const DexCode* code = meth->get_code();

    // Must have code
    if (!code) {
      m_num_culled_no_code++;
      return false;
    }

    // Must have at least two instructions
    const auto& insns = code->get_instructions();
    if (insns.size() < 2) {
      m_num_culled_too_short++;
      return false;
    }

    // Must satisfy one of the four "trivial invoke super" patterns
    if (!(are_opcs_equal(insns, s_return_invoke_super_void_opcs, 2) ||
      are_opcs_equal(insns, s_return_invoke_super_opcs, 3) ||
      are_opcs_equal(insns, s_return_invoke_super_wide_opcs, 3) ||
      are_opcs_equal(insns, s_return_invoke_super_obj_opcs, 3))) {
      m_num_culled_not_trivial++;
      return false;
    }

    // Must not be static
    if (meth->get_access() & ACC_STATIC) {
      m_num_culled_static++;
      return false;
    }

    // Must not be private
    if (meth->get_access() & ACC_PRIVATE) {
      m_num_private++;
    }

    // For non-void scenarios, capture move-result and return opcodes
    DexOpcode* move_res_opc = nullptr;
    DexOpcode* return_opc = nullptr;
    if (insns.size() == 3) {
      move_res_opc = insns[1];
      return_opc = insns[2];
    }
    m_num_trivial++;

    // Get invoked method
    const DexOpcode* opc = insns[0];
    const DexOpcodeMethod* mopc = static_cast<const DexOpcodeMethod*>(opc);
    DexMethod* invoked_meth = mopc->get_method();

    // Invoked method name must match
    if (meth->get_name() != invoked_meth->get_name()) {
      m_num_culled_name_differs++;
      return false;
    }

    // Invoked method proto must match
    if (meth->get_proto() != invoked_meth->get_proto()) {
      m_num_culled_proto_differs++;
      return false;
    }

    // Method return src register must match move-result dest register
    if (move_res_opc && return_opc &&
        move_res_opc->dest() != return_opc->src(0)) {
      m_num_culled_return_move_result_differs++;
      return false;
    }

    // Method args must pass through directly
    if (!do_invoke_meth_args_pass_through(meth, insns[0])) {
      m_num_culled_args_differs++;
      return false;
    }

    // If the invoked method does not have access flags, we can't operate
    // on it at all.
    if (!invoked_meth->is_def()) {
      m_num_culled_super_not_def++;
      return false;
    }
    // If invoked method is not public, make it public
    if (!is_public(invoked_meth)) {
      if (!invoked_meth->is_concrete()) {
        m_num_culled_super_is_non_public_sdk++;
        return false;
      }
      set_public(invoked_meth);
      m_num_relaxed_vis++;
    }

    return true;
  }

public:
  explicit DelSuper(const std::vector<DexClass*>& scope)
    : m_scope(scope),
      m_num_methods(0),
      m_num_passed(0),
      m_num_trivial(0),
      m_num_relaxed_vis(0),
      m_num_private(0),
      m_num_culled_no_code(0),
      m_num_culled_too_short(0),
      m_num_culled_not_trivial(0),
      m_num_culled_static(0),
      m_num_culled_name_differs(0),
      m_num_culled_proto_differs(0),
      m_num_culled_return_move_result_differs(0),
      m_num_culled_args_differs(0),
      m_num_culled_super_is_non_public_sdk(0),
      m_num_culled_super_not_def(0) {
  }

  void run(bool do_delete) {
    walk_methods(m_scope,
      [&](DexMethod* meth) {
        m_num_methods++;
        if (is_trivial_return_invoke_super(meth)) {
          TRACE(SUPER, 5, "Found trivial return invoke-super: %s\n",
            SHOW(meth));
          m_delmeths.push_back(meth);
          m_num_passed++;
        }
      });
    if (do_delete) {
      for (const auto meth : m_delmeths) {
        auto clazz = type_class(meth->get_class());
        always_assert(meth->is_virtual());
        clazz->get_vmethods().remove(meth);
        TRACE(SUPER, 5, "Deleted trivial return invoke-super: %s\n",
          SHOW(meth));
      }
    }
    print_stats(do_delete);
  }

  void print_stats(bool do_delete) {
    TRACE(SUPER, 1, "Examined %d total methods\n", m_num_methods);
    TRACE(SUPER, 1, "Found %d candidate trivial methods\n", m_num_trivial);
    TRACE(SUPER, 5, "Culled %d due to super not defined\n",
      m_num_culled_super_not_def);
    TRACE(SUPER, 5, "Culled %d due to method is static\n",
      m_num_culled_static);
    TRACE(SUPER, 5, "Culled %d due to method name doesn't match super\n",
      m_num_culled_name_differs);
    TRACE(SUPER, 5, "Culled %d due to method proto doesn't match super\n",
      m_num_culled_proto_differs);
    TRACE(SUPER, 5, "Culled %d due to method doesn't return move result\n",
      m_num_culled_return_move_result_differs);
    TRACE(SUPER, 5, "Culled %d due to method args doesn't match super\n",
      m_num_culled_args_differs);
    TRACE(SUPER, 5, "Culled %d due to non-public super method in sdk\n",
      m_num_culled_super_is_non_public_sdk);
    TRACE(SUPER, 1, "Found %d trivial return invoke-super methods\n",
      m_num_passed);
    if (do_delete) {
      TRACE(SUPER, 1, "Deleted %d trivial return invoke-super methods\n",
        m_num_passed);
      TRACE(SUPER, 1, "Promoted %d methods to public visibility\n",
        m_num_relaxed_vis);
    } else {
      TRACE(SUPER, 1, "Preview-only; not performing any changes.\n");
      TRACE(SUPER, 1, "Would delete %d trivial return invoke-super methods\n",
        m_num_passed);
      TRACE(SUPER, 1, "Would promote %d methods to public visibility\n",
        m_num_relaxed_vis);
    }
  }
};

}

void DelSuperPass::run_pass(DexClassesVector& dexen, PgoFiles& pgos) {
  const auto& scope = build_class_scope(dexen);
  DelSuper(scope).run(/* do_delete = */true);
}
