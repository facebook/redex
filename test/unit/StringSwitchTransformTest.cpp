/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "IRAssembler.h"
#include "InitClassesWithSideEffects.h"
#include "Purity.h"
#include "RedexTest.h"
#include "StringSwitchTransform.h"

namespace {

// A test double: reports a fixed score and counts apply() calls. apply() is a
// no-op on the CFG (so the recovered switch survives) -- the driver's budget
// cap must still guarantee it is applied at most once per recovered switch.
class MockTransform : public StringSwitchTransform {
 public:
  MockTransform(std::string name,
                std::optional<TransformScore> score,
                int* applied_counter)
      : m_name(std::move(name)),
        m_score(score),
        m_applied_counter(applied_counter) {}

  std::string_view name() const override { return m_name; }

  std::optional<TransformScore> evaluate(
      const StringSwitchCandidate& /*candidate*/) const override {
    return m_score;
  }

  void apply(const StringSwitchCandidate& /*candidate*/) const override {
    ++*m_applied_counter;
  }

 private:
  std::string m_name;
  std::optional<TransformScore> m_score;
  int* m_applied_counter;
};

// Runs the driver on `cfg`, supplying the (empty-scope) init-class info and
// side-effect-free method set that the pass builds once in production.
void run_driver(
    cfg::ControlFlowGraph& cfg,
    const std::vector<std::unique_ptr<StringSwitchTransform>>& transforms,
    DriverStats* stats) {
  init_classes::InitClassesWithSideEffects init_classes(
      /*scope=*/{}, /*create_init_class_insns=*/false);
  run_string_switch_transforms(/*method=*/nullptr, cfg, transforms,
                               get_pure_methods(), init_classes, stats);
}

// A minimal recoverable Form B (equals-chain) string switch.
std::unique_ptr<IRCode> two_case_switch() {
  return assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :first)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:second)
      (const-string "RES_second")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )");
}

} // namespace

class StringSwitchTransformTest : public RedexTest {};

TEST_F(StringSwitchTransformTest, is_better_prefers_performance_tier) {
  TransformScore size_big{TransformTier::SIZE, 1000};
  TransformScore perf_small{TransformTier::PERFORMANCE, 1};
  // PERFORMANCE beats SIZE regardless of magnitude.
  EXPECT_TRUE(is_better(perf_small, size_big));
  EXPECT_FALSE(is_better(size_big, perf_small));
}

TEST_F(StringSwitchTransformTest, is_better_breaks_ties_by_magnitude) {
  TransformScore hi{TransformTier::SIZE, 10};
  TransformScore lo{TransformTier::SIZE, 5};
  EXPECT_TRUE(is_better(hi, lo));
  EXPECT_FALSE(is_better(lo, hi));
  EXPECT_FALSE(is_better(hi, hi)); // strict
}

TEST_F(StringSwitchTransformTest, driver_picks_higher_tier) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied_size = 0;
  int applied_perf = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(std::make_unique<MockTransform>(
      "size", TransformScore{TransformTier::SIZE, 1000}, &applied_size));
  transforms.push_back(std::make_unique<MockTransform>(
      "perf", TransformScore{TransformTier::PERFORMANCE, 1}, &applied_perf));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied_perf, 1);
  EXPECT_EQ(applied_size, 0);
  EXPECT_EQ(stats.applied["perf"], 1u);
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_picks_higher_magnitude_within_tier) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied_hi = 0;
  int applied_lo = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(std::make_unique<MockTransform>(
      "lo", TransformScore{TransformTier::SIZE, 5}, &applied_lo));
  transforms.push_back(std::make_unique<MockTransform>(
      "hi", TransformScore{TransformTier::SIZE, 10}, &applied_hi));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied_hi, 1);
  EXPECT_EQ(applied_lo, 0);
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_skips_when_not_applicable) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(
      std::make_unique<MockTransform>("none", std::nullopt, &applied));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied, 0);
  EXPECT_TRUE(stats.applied.empty());
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_noop_with_empty_registry) {
  auto code = two_case_switch();
  code->build_cfg();
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);
  EXPECT_TRUE(stats.applied.empty());
  code->clear_cfg();
}
