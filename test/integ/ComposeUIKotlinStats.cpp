/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "PrintKotlinStats.h"
#include "RedexTest.h"
#include "Resolver.h"

class ComposeUIKotlinStatsTest : public RedexIntegrationTest {};

namespace {

using ::testing::AnyOf;
using ::testing::Eq;

TEST_F(ComposeUIKotlinStatsTest, test) {
  auto* klr = new PrintKotlinStats();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  PrintKotlinStats::Stats stats = klr->get_stats();

  // HelloWorldText, SuperTextPrinter, SubTextPrinter, getTestDefault
  EXPECT_EQ(stats.kotlin_composable_method, 4u);
  // Compose generates very messy dex code, this is a best-effort analysis based
  // on what I see:
  //
  // - SuperTextPrinter has 3 for the default param.
  // - Each of SuperTextPrinter, SubTextPrinter has 2 for the
  // changed param.
  // - 4 in an inlined updateChangedFlags method.
  // TODO(T233161282) The number changes depending on whether the Compose
  // pausable flag is set or not. For now let both cases pass, until the
  // pausable flag is set to be permanently on.
  EXPECT_THAT(stats.kotlin_composable_and_lit_insns, AnyOf(Eq(12u), Eq(15u)));
}
} // namespace
