/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DelSuper.h"

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>

#include "Walkers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"


namespace {

constexpr const char* METRIC_TOTAL_METHODS =
  "num_total_methods";
constexpr const char* METRIC_TRIVIAL_METHOD_CANDIDATES =
  "num_trivial_method_candidates";
constexpr const char* METRIC_REMOVED_TRIVIAL_METHODS =
  "num_removed_trivial_methods";
constexpr const char* METRIC_METHOD_RELAXED_VISIBILITY =
  "num_methods_relaxed_visibility";
constexpr const char* METRIC_CLASS_RELAXED_VISIBILITY =
    "num_class_relaxed_visibility";

static const IROpcode s_return_invoke_super_void_opcs[2] = {OPCODE_INVOKE_SUPER,
                                                            OPCODE_RETURN_VOID};

static const IROpcode s_return_invoke_super_opcs[3] = {
    OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT, OPCODE_RETURN};

static const IROpcode s_return_invoke_super_wide_opcs[3] = {
    OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT_WIDE, OPCODE_RETURN_WIDE};

static const IROpcode s_return_invoke_super_obj_opcs[3] = {
    OPCODE_INVOKE_SUPER, OPCODE_MOVE_RESULT_OBJECT, OPCODE_RETURN_OBJECT};

class DelSuper {

private:
  const std::vector<DexClass*>& m_scope;
  // trivial return invoke super method -> invoked super method
  std::unordered_map<DexMethod*, DexMethod*> m_delmeths;
  int m_num_methods;
  int m_num_passed;
  int m_num_trivial;
  int m_num_relaxed_vis;
  int m_num_cls_relaxed_vis;
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
  int m_num_culled_super_cls_non_public;
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
    const IRInstruction* insn) {
    redex_assert(insn->opcode() == OPCODE_INVOKE_SUPER);
    size_t src_idx{0};
    for (const auto& mie : InstructionIterable(
             meth->get_code()->get_param_instructions())) {
      auto load_param = mie.insn;
      if (load_param->dest() != insn->src(src_idx++)) {
        return false;
      }
    }
    return true;
  }

  bool are_opcs_equal(const std::vector<IRInstruction*> insns,
                      const IROpcode* opcs,
                      size_t opcs_len) {
    if (insns.size() != opcs_len) return false;
    for (size_t i = 0; i < opcs_len; ++i) {
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
   *
   * Returns the super method, or null if this is not a trivial return invoke
   * super.
   */
  DexMethod* get_trivial_return_invoke_super(const DexMethod* meth) {
    const auto* code = meth->get_code();

    // Must have code
    if (!code) {
      m_num_culled_no_code++;
      return nullptr;
    }

    // TODO: rewrite the following code to not require a random-access
    // container of instructions
    std::vector<IRInstruction*> insns;
    for (const auto& mie :
         InstructionIterable(meth->get_code())) {
      if (opcode::is_load_param(mie.insn->opcode())) {
        continue;
      }
      insns.emplace_back(mie.insn);
    }

    // Must have at least two instructions
    if (insns.size() < 2) {
      m_num_culled_too_short++;
      return nullptr;
    }

    // Must satisfy one of the four "trivial invoke super" patterns
    if (!(are_opcs_equal(insns, s_return_invoke_super_void_opcs, 2) ||
      are_opcs_equal(insns, s_return_invoke_super_opcs, 3) ||
      are_opcs_equal(insns, s_return_invoke_super_wide_opcs, 3) ||
      are_opcs_equal(insns, s_return_invoke_super_obj_opcs, 3))) {
      m_num_culled_not_trivial++;
      return nullptr;
    }

    // Must not be static
    if (is_static(meth)) {
      m_num_culled_static++;
      return nullptr;
    }

    // Must not be private
    if (is_private(meth)) {
      m_num_private++;
    }

    // For non-void scenarios, capture move-result and return opcodes
    IRInstruction* move_res_opc = nullptr;
    IRInstruction* return_opc = nullptr;
    if (insns.size() == 3) {
      move_res_opc = insns[1];
      return_opc = insns[2];
    }
    m_num_trivial++;

    // Get invoked method
    DexMethodRef* invoked_meth = insns[0]->get_method();

    // Invoked method name must match
    if (meth->get_name() != invoked_meth->get_name()) {
      m_num_culled_name_differs++;
      return nullptr;
    }

    // Invoked method proto must match
    if (meth->get_proto() != invoked_meth->get_proto()) {
      m_num_culled_proto_differs++;
      return nullptr;
    }

    // Method return src register must match move-result dest register
    if (move_res_opc && return_opc &&
        move_res_opc->dest() != return_opc->src(0)) {
      m_num_culled_return_move_result_differs++;
      return nullptr;
    }

    // Method args must pass through directly
    if (!do_invoke_meth_args_pass_through(meth, insns[0])) {
      m_num_culled_args_differs++;
      return nullptr;
    }

    // If the invoked method does not have access flags, we can't operate
    // on it at all.
    if (!invoked_meth->is_def()) {
      m_num_culled_super_not_def++;
      return nullptr;
    }
    auto meth_def = static_cast<DexMethod*>(invoked_meth);
    // If invoked method is not public, make it public
    if (!is_public(meth_def)) {
      if (!meth_def->is_concrete()) {
        m_num_culled_super_is_non_public_sdk++;
        return nullptr;
      }
      set_public(meth_def);
      m_num_relaxed_vis++;
    }

    auto cls = type_class(meth_def->get_class());
    if (!is_public(cls)) {
      if (cls->is_external()) {
        m_num_culled_super_cls_non_public++;
        return nullptr;
      }
      set_public(cls);
      m_num_cls_relaxed_vis++;
    }

    return meth_def;
  }

public:
  explicit DelSuper(const std::vector<DexClass*>& scope)
    : m_scope(scope),
      m_num_methods(0),
      m_num_passed(0),
      m_num_trivial(0),
      m_num_relaxed_vis(0),
      m_num_cls_relaxed_vis(0),
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
      m_num_culled_super_cls_non_public(0),
      m_num_culled_super_not_def(0) {
  }

  void run(bool do_delete, PassManager& mgr) {
    walk::methods(m_scope,
      [&](DexMethod* meth) {
        m_num_methods++;
        auto invoked_meth = get_trivial_return_invoke_super(meth);
        if (invoked_meth) {
          TRACE(SUPER, 5, "Found trivial return invoke-super: %s\n",
            SHOW(meth));
          m_delmeths.emplace(meth, invoked_meth);
          m_num_passed++;
        }
      });
    if (do_delete) {
      // we technically don't have to rewrite the opcodes -- we could just
      // remove the method declarations and the runtime semantics would be
      // unchanged -- but this ensures that we have no more references to
      // that method_id and can avoid emitting it in the dex output.
      walk::opcodes(m_scope,
                   [](DexMethod* meth) { return true; },
                   [&](DexMethod* meth, IRInstruction* insn) {
                     if (is_invoke(insn->opcode())) {
                       if (!insn->get_method()->is_def()) {
                         return;
                       }
                       auto method =
                           static_cast<DexMethod*>(insn->get_method());
                       while (m_delmeths.count(method)) {
                         method = m_delmeths.at(method);
                       }
                       insn->set_method(method);
                     }
                   });
      for (const auto& pair : m_delmeths) {
        auto meth = pair.first;
        auto clazz = type_class(meth->get_class());
        always_assert(meth->is_virtual());
        clazz->remove_method(meth);
        DexMethod::erase_method(meth);
        TRACE(SUPER, 5, "Deleted trivial return invoke-super: %s\n",
          SHOW(meth));
      }
    }
    print_stats(do_delete, mgr);
  }

  void print_stats(bool do_delete, PassManager& mgr) {
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
    TRACE(SUPER, 5, "Culled %d due to non-public super class in sdk\n",
      m_num_culled_super_cls_non_public);
    TRACE(SUPER, 1, "Found %d trivial return invoke-super methods\n",
      m_num_passed);
    if (do_delete) {
      TRACE(SUPER, 1, "Deleted %d trivial return invoke-super methods\n",
        m_num_passed);
      TRACE(SUPER, 1, "Promoted %d methods to public visibility\n",
        m_num_relaxed_vis);
      TRACE(SUPER, 1, "Promoted %d classes to public visibility\n",
        m_num_cls_relaxed_vis);
    } else {
      TRACE(SUPER, 1, "Preview-only; not performing any changes.\n");
      TRACE(SUPER, 1, "Would delete %d trivial return invoke-super methods\n",
        m_num_passed);
      TRACE(SUPER, 1, "Would promote %d methods to public visibility\n",
        m_num_relaxed_vis);
      TRACE(SUPER, 1, "Would promote %d classes to public visibility\n",
        m_num_cls_relaxed_vis);
    }

    mgr.incr_metric(METRIC_TOTAL_METHODS, m_num_methods);
    mgr.incr_metric(METRIC_TRIVIAL_METHOD_CANDIDATES, m_num_trivial);
    mgr.incr_metric(METRIC_REMOVED_TRIVIAL_METHODS, m_num_passed);
    mgr.incr_metric(METRIC_METHOD_RELAXED_VISIBILITY, m_num_relaxed_vis);
    mgr.incr_metric(METRIC_CLASS_RELAXED_VISIBILITY, m_num_cls_relaxed_vis);
  }
};

}

void DelSuperPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& /* conf */,
                            PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  DelSuper(scope).run(/* do_delete = */true, mgr);
}

static DelSuperPass s_pass;
