/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <vector>

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "PrintKotlinStats.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

class ComposeUIKotlinStatsTest : public RedexIntegrationTest {};

namespace {

TEST_F(ComposeUIKotlinStatsTest, test) {
  auto klr = new PrintKotlinStats();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  PrintKotlinStats::Stats stats = klr->get_stats();

  // Compose generates very messy dex code, this is a best-effort analysis based
  // on what I see:
  //
  // - SuperTextPrinter has 3 for the default param.
  // - Each of SuperTextPrinter, SubTextPrinter has 2 for the
  // changed param.
  // - 4 in an inlined updateChangedFlags method.
  EXPECT_EQ(stats.kotlin_composable_and_lit_insns, 12u);
}
} // namespace
