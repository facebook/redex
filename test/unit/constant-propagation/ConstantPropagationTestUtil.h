/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <utility>

#include "ConstantPropagation.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

struct ConstantPropagationTest : public RedexTest {};

inline void do_const_prop(
    IRCode* code,
    const std::function<void(const IRInstruction*, ConstantEnvironment*)>&
        insn_analyzer = cp::ConstantPrimitiveAnalyzer()) {
  code->build_cfg(/* editable */ false);
  cp::intraprocedural::FixpointIterator intra_cp(code->cfg(), insn_analyzer);
  intra_cp.run(ConstantEnvironment());
  cp::Transform::Config transform_config;
  cp::Transform tf(transform_config);
  tf.apply(intra_cp, cp::WholeProgramState(), code);
}
