/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MetricsSink.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Stands in for ScopedMetrics: records what a real sink would forward to a
// PassManager, without needing one.
class RecordingSink : public MetricsSink {
 public:
  std::vector<std::pair<std::string, int64_t>> metrics;

 protected:
  void report_metric(const std::string& key, int64_t value) override {
    metrics.emplace_back(key, value);
  }
};

} // namespace

TEST(MetricsSinkTest, unscoped_key_is_reported_verbatim) {
  RecordingSink sink;
  sink.set_metric("count", 7);

  ASSERT_EQ(sink.metrics.size(), 1u);
  EXPECT_EQ(sink.metrics[0].first, "count");
  EXPECT_EQ(sink.metrics[0].second, 7);
}

TEST(MetricsSinkTest, nested_scopes_prefix_the_key) {
  RecordingSink sink;
  {
    auto outer = sink.scope("a");
    sink.set_metric("in_outer", 1);
    {
      auto inner = sink.scope("b");
      sink.set_metric("in_inner", 2);
    }
    // The inner scope is gone again.
    sink.set_metric("after_inner", 3);
  }
  sink.set_metric("after_outer", 4);

  std::vector<std::pair<std::string, int64_t>> expected{{"a.in_outer", 1},
                                                        {"a.b.in_inner", 2},
                                                        {"a.after_inner", 3},
                                                        {"after_outer", 4}};
  EXPECT_EQ(sink.metrics, expected);
}

TEST(MetricsSinkTest, moved_from_scope_does_not_pop_twice) {
  RecordingSink sink;
  {
    auto outer = sink.scope("a");
    auto moved = std::move(outer);
    sink.set_metric("key", 1);
  }
  // Had the moved-from Scope popped as well, this key would still be prefixed
  // by a leftover segment, or the pop would have asserted.
  sink.set_metric("after", 2);

  std::vector<std::pair<std::string, int64_t>> expected{{"a.key", 1},
                                                        {"after", 2}};
  EXPECT_EQ(sink.metrics, expected);
}

TEST(MetricsSinkTest, arithmetic_values_are_narrowed_to_int64) {
  RecordingSink sink;
  sink.set_metric("from_double", 2.75);
  sink.set_metric("from_size_t", static_cast<size_t>(9));
  sink.set_metric("from_bool", true);

  std::vector<std::pair<std::string, int64_t>> expected{
      {"from_double", 2}, {"from_size_t", 9}, {"from_bool", 1}};
  EXPECT_EQ(sink.metrics, expected);
}

TEST(MetricsSinkTest, atomic_values_are_loaded) {
  RecordingSink sink;
  std::atomic<size_t> counter{42};
  sink.set_metric("counter", counter);

  ASSERT_EQ(sink.metrics.size(), 1u);
  EXPECT_EQ(sink.metrics[0].first, "counter");
  EXPECT_EQ(sink.metrics[0].second, 42);
}
