/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassOrderSample.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace class_order_sample;

namespace {

ClassEntry entry(std::string_view name,
                 Segment segment,
                 uint32_t dex = 1,
                 bool is_generated = false) {
  return ClassEntry{name, is_generated, segment, dex};
}

// name_hash -> (seg_index, dex) for a segment's sampled classes.
std::map<uint32_t, std::pair<uint32_t, uint32_t>> by_hash(
    const SegmentSample& seg) {
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> m;
  for (const auto& sc : seg.sampled) {
    m[sc.name_hash] = {sc.seg_index, sc.dex};
  }
  return m;
}

} // namespace

TEST(ClassOrderSampleTest, NameHashIsDeterministicAndDistinct) {
  EXPECT_EQ(name_hash("Lcom/foo/Bar;"), name_hash("Lcom/foo/Bar;"));
  EXPECT_NE(name_hash("Lcom/foo/Bar;"), name_hash("Lcom/foo/Baz;"));
}

TEST(ClassOrderSampleTest, ThresholdKeepsAllWhenAtOrUnderCap) {
  const uint64_t k_space = uint64_t(1) << 32;
  EXPECT_EQ(threshold(/* num_classes */ 0, /* cap */ 1000), k_space);
  EXPECT_EQ(threshold(500, 1000), k_space);
  EXPECT_EQ(threshold(1000, 1000), k_space);
}

TEST(ClassOrderSampleTest, ThresholdScalesDownWhenOverCap) {
  const uint64_t k_space = uint64_t(1) << 32;
  EXPECT_EQ(threshold(2000, 1000), k_space / 2);
  EXPECT_EQ(threshold(4000, 1000), k_space / 4);
}

TEST(ClassOrderSampleTest, ThresholdDoesNotOverflowForHugeCap) {
  const uint64_t k_space = uint64_t(1) << 32;
  // cap above 2^32 makes the naive k_space * cap product overflow uint64_t; the
  // 128-bit-widened computation must still yield the exact scaled threshold.
  const uint64_t huge_cap = (uint64_t(1) << 40); // 2^40, well past 2^32
  EXPECT_EQ(threshold(/* num_classes */ huge_cap * 4, huge_cap), k_space / 4);
  // Result stays strictly below the whole space whenever num_classes > cap.
  EXPECT_LT(threshold(huge_cap + 1, huge_cap), k_space);
}

TEST(ClassOrderSampleTest, ClassifyPrecedence) {
  // Dynamically-dead wins over everything, including betamap and dex 0.
  EXPECT_EQ(classify(/* dead */ true, /* betamap */ true, /* dex */ 0),
            Segment::DYNAMICALLY_DEAD);
  EXPECT_EQ(classify(true, false, 5), Segment::DYNAMICALLY_DEAD);

  // Betamap (when not dead) wins over dex 0: coldstart classes in the primary
  // dex must stay in the betamap region.
  EXPECT_EQ(classify(false, true, 0), Segment::BETAMAP);
  EXPECT_EQ(classify(false, true, 7), Segment::BETAMAP);

  // Non-dead, non-betamap residue of dex 0 is PRIMARY.
  EXPECT_EQ(classify(false, false, 0), Segment::PRIMARY);

  // Everything else is COLD.
  EXPECT_EQ(classify(false, false, 1), Segment::COLD);
}

TEST(ClassOrderSampleTest, SegmentAssignmentWithDensePerSegmentIndices) {
  // Emission order across segments; a generated class and a dead class must not
  // consume a seg_index in any sampled segment.
  std::vector<ClassEntry> classes = {
      entry("LP;", Segment::PRIMARY, /* dex */ 0),
      entry("LB1;", Segment::BETAMAP, /* dex */ 1),
      entry("LGen;", Segment::BETAMAP, /* dex */ 1, /* generated */ true),
      entry("LB2;", Segment::BETAMAP, /* dex */ 1),
      entry("LC1;", Segment::COLD, /* dex */ 2),
      entry("LDead;", Segment::DYNAMICALLY_DEAD, /* dex */ 5),
      entry("LC2;", Segment::COLD, /* dex */ 2),
  };
  auto sample = build(classes, /* cold_cap */ 1000);

  EXPECT_EQ(sample.primary.num_classes, 1u);
  EXPECT_EQ(sample.betamap.num_classes, 2u);
  EXPECT_EQ(sample.cold.num_classes, 2u);
  EXPECT_EQ(sample.dead_num_classes, 1u);

  // Kept in full (under cap), dense seg_index within each segment, dex
  // recorded.
  auto primary = by_hash(sample.primary);
  EXPECT_EQ(primary.at(name_hash("LP;")), std::make_pair(0u, 0u));

  auto betamap = by_hash(sample.betamap);
  EXPECT_EQ(betamap.at(name_hash("LB1;")), std::make_pair(0u, 1u));
  EXPECT_EQ(betamap.at(name_hash("LB2;")), std::make_pair(1u, 1u));

  auto cold = by_hash(sample.cold);
  EXPECT_EQ(cold.at(name_hash("LC1;")), std::make_pair(0u, 2u));
  EXPECT_EQ(cold.at(name_hash("LC2;")), std::make_pair(1u, 2u));
}

TEST(ClassOrderSampleTest, SkipsGeneratedAndEmptyNamedClasses) {
  std::vector<ClassEntry> classes = {
      entry("LA;", Segment::COLD),
      entry("", Segment::COLD),
      entry("LGen;", Segment::COLD, /* dex */ 1, /* generated */ true),
      entry("LB;", Segment::COLD),
  };
  auto sample = build(classes, /* cold_cap */ 1000);

  // Only LA; and LB; are kept; empty-named and generated classes are excluded
  // from the population and do not consume an index.
  EXPECT_EQ(sample.cold.num_classes, 2u);
  auto cold = by_hash(sample.cold);
  EXPECT_EQ(cold.at(name_hash("LA;")).first, 0u);
  EXPECT_EQ(cold.at(name_hash("LB;")).first, 1u);
}

TEST(ClassOrderSampleTest, DeadClassesCountedButNotSampled) {
  std::vector<ClassEntry> classes = {
      entry("LD1;", Segment::DYNAMICALLY_DEAD, /* dex */ 9),
      entry("LD2;", Segment::DYNAMICALLY_DEAD, /* dex */ 9),
      entry("LC1;", Segment::COLD, /* dex */ 2),
  };
  auto sample = build(classes, /* cold_cap */ 1000);
  EXPECT_EQ(sample.dead_num_classes, 2u);
  EXPECT_TRUE(sample.betamap.sampled.empty());
  EXPECT_TRUE(sample.primary.sampled.empty());
  EXPECT_EQ(sample.cold.sampled.size(), 1u);
}

TEST(ClassOrderSampleTest, ColdIsSampledWhileHotSegmentsKeptInFull) {
  constexpr int kCold = 100000;
  constexpr int kBetamap = 50;
  constexpr int kColdCap = 1000;

  // Own the strings so the string_views stay valid for build().
  std::vector<std::string> names;
  names.reserve(kCold + kBetamap);
  for (int i = 0; i < kCold; i++) {
    names.push_back("Lcom/cold/Class" + std::to_string(i) + ";");
  }
  for (int i = 0; i < kBetamap; i++) {
    names.push_back("Lcom/hot/Class" + std::to_string(i) + ";");
  }

  std::vector<ClassEntry> classes;
  classes.reserve(kCold + kBetamap);
  for (int i = 0; i < kCold; i++) {
    classes.push_back(entry(names[i], Segment::COLD, /* dex */ 3));
  }
  for (int i = 0; i < kBetamap; i++) {
    classes.push_back(entry(names[kCold + i], Segment::BETAMAP, /* dex */ 1));
  }

  auto sample = build(classes, kColdCap);

  // Betamap is kept in full regardless of cap.
  EXPECT_EQ(sample.betamap.num_classes, (uint32_t)kBetamap);
  EXPECT_EQ(sample.betamap.sampled.size(), (uint32_t)kBetamap);

  // Cold is sampled down to ~cap; wide bounds keep this non-flaky.
  EXPECT_EQ(sample.cold.num_classes, (uint32_t)kCold);
  EXPECT_GT(sample.cold.sampled.size(), 500u);
  EXPECT_LT(sample.cold.sampled.size(), 1500u);
}

TEST(ClassOrderSampleTest, IsDeterministic) {
  std::vector<ClassEntry> classes = {
      entry("LA;", Segment::BETAMAP),
      entry("LB;", Segment::COLD),
      entry("LC;", Segment::COLD),
  };
  auto a = build(classes, 1000);
  auto b = build(classes, 1000);
  EXPECT_EQ(a.betamap.sampled, b.betamap.sampled);
  EXPECT_EQ(a.cold.sampled, b.cold.sampled);
}
