/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <utility>

#include "ConstantPropagationPass.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

struct ConstantPropagationTest : public RedexTest {};

inline void do_const_prop(
    IRCode* code,
    const std::function<void(const IRInstruction*, ConstantEnvironment*)>&
        insn_analyzer = cp::ConstantPrimitiveAnalyzer(),
    const cp::Transform::Config& transform_config = cp::Transform::Config(),
    bool not_legacy = false) {
  code->build_cfg(true);
  code->cfg().calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(code->cfg(), insn_analyzer);
  intra_cp.run(ConstantEnvironment());
  cp::Transform tf(transform_config);
  if (not_legacy) {
    tf.apply(intra_cp, code->cfg(), nullptr, nullptr);
  } else {
    tf.apply_legacy(
        intra_cp, cp::WholeProgramState(), code->cfg(), nullptr, nullptr);
  }
  code->clear_cfg();
}
