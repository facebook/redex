/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

struct ConstantPropagationTest : public RedexTest {};

inline void do_const_prop(
    IRCode* code,
    std::function<void(const IRInstruction*, ConstantEnvironment*)>
        insn_analyzer = cp::ConstantPrimitiveAnalyzer()) {
  code->build_cfg();
  cp::intraprocedural::FixpointIterator intra_cp(code->cfg(), insn_analyzer);
  intra_cp.run(ConstantEnvironment());
  cp::Transform::Config transform_config;
  cp::Transform tf(transform_config);
  tf.apply(intra_cp, cp::WholeProgramState(), code);
}
