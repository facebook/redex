/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "IRInstruction.h"
#include "Match.h"
#include "MatchFlow.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

#include <iterator>
#include <vector>

namespace mf {
namespace {

class MatchFlowTest : public RedexTest {};

using test_range = result_t::range<std::vector<IRInstruction*>::const_iterator>;

TEST_F(MatchFlowTest, EmptyRange) {
  auto empty = test_range::empty();
  EXPECT_EQ(0, std::distance(empty.begin(), empty.end()));
}

TEST_F(MatchFlowTest, RangeUnique) {
  auto add = std::make_unique<IRInstruction>(OPCODE_ADD_INT);
  auto sub = std::make_unique<IRInstruction>(OPCODE_SUB_INT);

  std::vector<IRInstruction*> zero;
  std::vector<IRInstruction*> one{add.get()};
  std::vector<IRInstruction*> two{add.get(), sub.get()};

  test_range rzero{zero.cbegin(), zero.cend()};
  test_range rone{one.cbegin(), one.cend()};
  test_range rtwo{two.cbegin(), two.cend()};

  EXPECT_EQ(rzero.unique(), nullptr);
  EXPECT_EQ(rone.unique(), add.get());
  EXPECT_EQ(rtwo.unique(), nullptr);
}

TEST_F(MatchFlowTest, Empty) {
  flow_t f;

  auto add = f.insn(m::add_int_());
  add.src(0, add);

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (:L)
    (add-int v0 v0 v1)
    (goto :L)
  ))");

  cfg::ScopedCFG cfg{code.get()};

  auto res = f.find(*cfg, add);

  auto insns = res.matching(add);
  EXPECT_EQ(insns.begin(), insns.end());
}

} // namespace
} // namespace mf
