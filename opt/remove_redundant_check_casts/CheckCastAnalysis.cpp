/*
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

CheckCastReplacements
CheckCastAnalysis::collect_redundant_checks_replacement() {
  CheckCastReplacements redundant_check_casts;

  if (!m_method || !m_method->get_code()) {
    return redundant_check_casts;
  }

  auto* code = m_method->get_code();
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(m_method);
  auto& envs = inference.get_type_environments();

  auto iterable = cfg::InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    cfg::Block* block = it.block();
    IRInstruction* insn = it->insn;
    if (insn->opcode() != OPCODE_CHECK_CAST) {
      continue;
    }

    auto reg = insn->src(0);
    auto& env = envs.at(insn);
    auto type = env.get_type(reg);
    auto dex_type = env.get_dex_type(reg);
    if (type.equals(type_inference::TypeDomain(ZERO)) ||
        (dex_type && type::check_cast(*dex_type, insn->get_type()))) {
      auto src = insn->src(0);
      auto move = cfg.move_result_of(it);
      if (move.is_end()) {
        continue;
      }

      auto dst = move->insn->dest();
      if (src == dst) {
        redundant_check_casts.emplace_back(block, insn, boost::none);
      } else {
        auto new_move = new IRInstruction(OPCODE_MOVE_OBJECT);
        new_move->set_src(0, src);
        new_move->set_dest(dst);
        redundant_check_casts.emplace_back(
            block, insn, boost::optional<IRInstruction*>(new_move));
      }
    }
  }

  return redundant_check_casts;
}

} // namespace impl

} // namespace check_casts
