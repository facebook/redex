/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Armv7JitLoopChecker.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

using LoopScore = Armv7JitLoopChecker::LoopScore;
using Weighting = Armv7JitLoopChecker::Weighting;

class Armv7JitLoopCheckerTest : public RedexTest {};

namespace {

// The shape of a generated JSON parser: each case writes one local and jumps to
// a merge block (which does real work, so the CFG keeps it), and the locals are
// read after the loop. v2..v4 are carried; the merge has the three cases plus
// the switch default as predecessors.
constexpr const char* kParserLoop = R"(
  (
    (load-param v5)
    (const v2 0)
    (const v3 0)
    (const v4 0)
    (:loop)
    (if-eqz v5 :done)
    (switch v5 (:c0 :c1 :c2))
    (goto :merge)
    (:c0 0)
    (const v2 1)
    (goto :merge)
    (:c1 1)
    (const v3 1)
    (goto :merge)
    (:c2 2)
    (const v4 1)
    (goto :merge)
    (:merge)
    (invoke-static (v5) "LFoo;.skip:(I)V")
    (goto :loop)
    (:done)
    (invoke-static (v2 v3 v4) "LFoo;.use:(III)V")
    (return-void)
  )
)";

// Same loop, but nothing written in it is read after it, so nothing is carried.
constexpr const char* kNothingCarried = R"(
  (
    (load-param v5)
    (:loop)
    (if-eqz v5 :done)
    (switch v5 (:c0 :c1 :c2))
    (goto :merge)
    (:c0 0)
    (const v2 1)
    (goto :merge)
    (:c1 1)
    (const v3 1)
    (goto :merge)
    (:c2 2)
    (const v4 1)
    (goto :merge)
    (:merge)
    (invoke-static (v5) "LFoo;.skip:(I)V")
    (goto :loop)
    (:done)
    (return-void)
  )
)";

constexpr const char* kNoLoop = R"(
  (
    (load-param v0)
    (if-eqz v0 :done)
    (invoke-static (v0) "LFoo;.skip:(I)V")
    (:done)
    (return-void)
  )
)";

std::optional<LoopScore> score(const char* body,
                               const Weighting& weighting = {}) {
  auto code = assembler::ircode_from_string(body);
  code->build_cfg();
  return Armv7JitLoopChecker::score_method(code->cfg(), weighting);
}

} // namespace

TEST_F(Armv7JitLoopCheckerTest, parserLoopCountsCarriedValuesAndMergeEdges) {
  auto s = score(kParserLoop);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->carried_values, 3);
  EXPECT_EQ(s->merge_in_degree, 4);
  EXPECT_EQ(s->score, 12);
}

TEST_F(Armv7JitLoopCheckerTest, valuesNotReadAfterTheLoopAreNotCarried) {
  auto s = score(kNothingCarried);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->carried_values, 0);
  EXPECT_EQ(s->score, 0);
}

TEST_F(Armv7JitLoopCheckerTest, methodWithoutLoopHasNoScore) {
  EXPECT_FALSE(score(kNoLoop));
}

// Past large_frame_carried_values, the score grows by one extra copy per copy
// for every large_frame_weight_divisor further carried values.
TEST_F(Armv7JitLoopCheckerTest, largeFramesWeightTheScore) {
  Weighting weighting;
  weighting.large_frame_carried_values = 2;
  weighting.large_frame_weight_divisor = 1;
  auto s = score(kParserLoop, weighting);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->score, 12 + 12 * (3 - 2));
}
