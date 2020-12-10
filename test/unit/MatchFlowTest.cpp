/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Match.h"
#include "MatchFlow.h"
#include "RedexTest.h"

namespace mf {
namespace {

class MatchFlowTest : public RedexTest {};

TEST_F(MatchFlowTest, Compiles) {
  flow_t f;

  auto add = f.insn(m::add_int_());
  add.src(0, add);
}

} // namespace
} // namespace mf
