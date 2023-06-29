/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <utility>

#include "ConstantPropagationPass.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

struct ConstantPropagationTest : public RedexTest {};

enum class ConstPropMode {
  OnlyForwardTargets,
  DontForwardTargets,
  All,
};

inline void do_const_prop(
    IRCode* code,
    const std::function<void(const IRInstruction*, ConstantEnvironment*)>&
        insn_analyzer = cp::ConstantPrimitiveAnalyzer(),
    const cp::Transform::Config& transform_config = cp::Transform::Config(),
    ConstPropMode mode = ConstPropMode::DontForwardTargets) {
  code->build_cfg(true);
  cp::intraprocedural::FixpointIterator intra_cp(code->cfg(), insn_analyzer);
  intra_cp.run(ConstantEnvironment());
  cp::Transform tf(transform_config);
  auto constants_and_prune_unreachable = [&]() {
    tf.legacy_apply_constants_and_prune_unreachable(
        intra_cp, cp::WholeProgramState(), code->cfg(), nullptr, nullptr);
  };
  auto forward_target = [&]() {
    tf.legacy_apply_forward_targets(
        intra_cp, code->cfg(), false, nullptr, nullptr, nullptr);
  };
  switch (mode) {
  case ConstPropMode::OnlyForwardTargets:
    forward_target();
    break;
  case ConstPropMode::DontForwardTargets:
    constants_and_prune_unreachable();
    break;
  case ConstPropMode::All:
    constants_and_prune_unreachable();
    code->cfg().simplify();
    intra_cp.clear_switch_succ_cache();
    forward_target();
    break;
  default:
    not_reached();
  }
  code->clear_cfg();
}
