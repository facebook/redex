/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "Resolver.h"
#include "Show.h"

namespace npe {

class NullPointerExceptionCreator {
 private:
  // We need some temp registers, and type and method references, if a
  // null-pointer-exception transformation is applied to a cfg. We lazily
  // initialize and then cache this state, as we can reuse it across
  // transformations for the same cfg.
  struct State {
    reg_t string_reg;
    reg_t exception_reg;
    DexType* npe_type;
    DexMethodRef* npe_init_method;
  };

  cfg::ControlFlowGraph* m_cfg;
  std::unique_ptr<State> m_state;

 public:
  explicit NullPointerExceptionCreator(cfg::ControlFlowGraph* cfg)
      : m_cfg(cfg) {}

  std::vector<IRInstruction*> get_insns(
      IRInstruction* implicitly_throwing_npe_insn) {
    // clang-format off
    // const-string "<message>"
    // move-result-pseudo-object v1
    // new-instance Ljava/lang/NullPointerException;
    // move-result-pseudo-object v0
    // invoke-direct {v0, v1}, Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V
    // throw v0
    // clang-format on

    if (!m_state) {
      m_state.reset(
          new State({m_cfg->allocate_temp(), m_cfg->allocate_temp(),
                     DexType::make_type("Ljava/lang/NullPointerException;"),
                     DexMethod::make_method(
                         "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/"
                         "String;)V")}));
    }

    // The message mentions the instance member, an access to which causes the
    // null-pointer-exception.
    std::string str;
    if (implicitly_throwing_npe_insn->has_field()) {
      auto resolved_field = resolve_field(
          implicitly_throwing_npe_insn->get_field(), FieldSearch::Instance);
      if (resolved_field != nullptr) {
        str = resolved_field->get_simple_deobfuscated_name();
      } else {
        str = implicitly_throwing_npe_insn->get_field()->get_name()->str_copy();
      }
    } else if (implicitly_throwing_npe_insn->has_method()) {
      auto resolved_method =
          resolve_method(implicitly_throwing_npe_insn->get_method(),
                         opcode_to_search(implicitly_throwing_npe_insn));
      if (resolved_method != nullptr) {
        str = resolved_method->get_simple_deobfuscated_name();
      } else {
        str =
            implicitly_throwing_npe_insn->get_method()->get_name()->str_copy();
      }
    } else if (opcode::is_an_aput(implicitly_throwing_npe_insn->opcode()) ||
               opcode::is_an_aget(implicitly_throwing_npe_insn->opcode())) {
      str = "array access";
    } else {
      // if there is no field or method, we show the instruction opcode,
      // e.g. "monitor-enter".
      str = show(implicitly_throwing_npe_insn->opcode());
      std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return c == '_' ? '-' : std::tolower(c);
      });
    }

    auto const_inst = (new IRInstruction(OPCODE_CONST_STRING))
                          ->set_string(DexString::make_string(str));

    auto const_move_result_pseudo_object_insn =
        (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
            ->set_dest(m_state->string_reg);

    auto new_inst =
        (new IRInstruction(OPCODE_NEW_INSTANCE))->set_type(m_state->npe_type);

    auto new_move_result_pseudo_object_insn =
        (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
            ->set_dest(m_state->exception_reg);

    auto invoke_insn = (new IRInstruction(OPCODE_INVOKE_DIRECT))
                           ->set_method(m_state->npe_init_method)
                           ->set_srcs_size(2)
                           ->set_src(0, m_state->exception_reg)
                           ->set_src(1, m_state->string_reg);
    IRInstruction* throw_inst =
        (new IRInstruction(OPCODE_THROW))->set_src(0, m_state->exception_reg);

    return {const_inst,  const_move_result_pseudo_object_insn,
            new_inst,    new_move_result_pseudo_object_insn,
            invoke_insn, throw_inst};
  }
};

} // namespace npe
