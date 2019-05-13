/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CheckCastAnalysis.h"

#include "DexUtil.h"
#include "TypeInference.h"

namespace check_casts {

namespace impl {

const CheckCastReplacements
CheckCastAnalysis::collect_redundant_checks_replacement() {
  CheckCastReplacements redundant_check_casts;

  if (!m_method || !m_method->get_code()) {
    return redundant_check_casts;
  }

  auto* code = m_method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(m_method);
  auto& envs = inference.get_type_environments();

  for (auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    if (insn->opcode() != OPCODE_CHECK_CAST) {
      continue;
    }
    auto dex_type = envs.at(insn).get_dex_type(insn->src(0));
    if (dex_type && check_cast(*dex_type, insn->get_type())) {
      auto src = insn->src(0);
      auto it = code->iterator_to(mie);
      auto dst = ir_list::move_result_pseudo_of(it)->dest();
      if (src == dst) {
        redundant_check_casts.emplace_back(&mie, boost::none);
      } else {
        auto new_move = new IRInstruction(OPCODE_MOVE_OBJECT);
        new_move->set_src(0, src);
        new_move->set_dest(dst);
        redundant_check_casts.emplace_back(
            &mie, boost::optional<IRInstruction*>(new_move));
      }
    }
  }

  return redundant_check_casts;
}

} // namespace impl

} // namespace check_casts
