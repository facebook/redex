/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <vector>

#include <gtest/gtest.h>

#include "PrintKotlinStats.h"
#include "RedexTest.h"

class AtomicFieldUpdaterKotlinStatsTest : public RedexIntegrationTest {};

namespace {

// The counters run over real javac output rather than assembled IR, so they
// are measured against the `newUpdater` and operation shapes a compiler
// actually emits.
//
// Counts come from AtomicFieldUpdaterLowering.java:
//   newUpdater  -- one per flavor in Holder's <clinit>.
//   operations  -- provenHolder:        set, get        (2)
//                  primitives:          I.set, I.compareAndSet, I.get,
//                                       L.set, L.get    (5)
//                  unprovenHolder:      get             (1)
//                  unresolvableUpdater: get             (1)
//                  D8's forwarder                       (2)
//
// The last entry is why the total is 11 and not 9: D8 rewrites the
// reference-flavored `compareAndSet` into a synthetic
// `$$ExternalSyntheticBackportWithForwarding0`, so the call at the original
// site is no longer on the updater type, while the forwarder body invokes
// `compareAndSet` on both of its paths. The counters measure calls against the
// updater API wherever they end up, which is the right thing for sizing the
// opportunity -- and is exactly the kind of accounting an assembled-IR test
// cannot check, since it never sees a desugarer.
TEST_F(AtomicFieldUpdaterKotlinStatsTest, countsUpdaterUsage) {
  auto* pass = new PrintKotlinStats();
  std::vector<Pass*> passes{pass};
  run_passes(passes);

  PrintKotlinStats::Stats stats = pass->get_stats();

  EXPECT_EQ(stats.atomic_field_updater_newupdater_insns, 3u);
  EXPECT_EQ(stats.atomic_field_updater_op_insns, 11u);
}

} // namespace
