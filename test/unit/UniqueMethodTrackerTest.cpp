/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UniqueMethodTracker.h"

#include <gmock/gmock.h>

#include "IRAssembler.h"
#include "RedexTest.h"

using ::testing::IsNull;
using ::testing::SizeIs;

class UniqueMethodTrackerTest : public RedexTest, public UniqueMethodTracker {
 protected:
  void SetUp() override {
    RedexTest::SetUp();

    // Insert baseline methods with distinct code to seed the tracker.
    auto* baseline1 = assembler::method_from_string(R"(
      (method (public static) "LFoo;.baseline1:()V"
        (
          (return-void)
        )
      )
    )");

    auto* baseline2 = assembler::method_from_string(R"(
      (method (public static) "LFoo;.baseline2:()V"
        (
          (const v0 0)
          (return-void)
        )
      )
    )");

    baseline1->get_code()->build_cfg();
    baseline2->get_code()->build_cfg();

    const auto [rep1, inserted1] = this->insert(baseline1);
    ASSERT_TRUE(inserted1);
    ASSERT_EQ(rep1, baseline1);

    const auto [rep2, inserted2] = this->insert(baseline2);
    ASSERT_TRUE(inserted2);
    ASSERT_EQ(rep2, baseline2);

    ASSERT_THAT(*this, SizeIs(2u));
  }
};

TEST_F(UniqueMethodTrackerTest, UniqueMethodTrackerIdenticalCode) {
  // Two methods with identical code should be deduplicated.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar1:()I"
      (
        (const v0 42)
        (return v0)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar2:()I"
      (
        (const v0 42)
        (return v0)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  const auto [rep1, inserted1] = this->insert(method1);
  EXPECT_TRUE(inserted1) << "Expected method1 inserted as new unique code";
  EXPECT_EQ(rep1, method1) << "Expected representative equals inserted method";

  const auto [rep2, inserted2] = this->insert(method2);
  EXPECT_FALSE(inserted2)
      << "Expected method2 not inserted (duplicate of method1)";
  EXPECT_EQ(rep2, method1) << "Expected same representative for duplicate code";

  EXPECT_THAT(*this, SizeIs(3u));
}

TEST_F(UniqueMethodTrackerTest, UniqueMethodTrackerDifferentCode) {
  // Two methods with different code should both be tracked.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.add:(II)I"
      (
        (load-param v0)
        (load-param v1)
        (add-int v2 v0 v1)
        (return v2)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.sub:(II)I"
      (
        (load-param v0)
        (load-param v1)
        (sub-int v2 v0 v1)
        (return v2)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  const auto [rep1, inserted1] = this->insert(method1);
  EXPECT_TRUE(inserted1) << "Expected method1 inserted as new unique code";
  EXPECT_EQ(rep1, method1) << "Expected representative equals inserted method";

  const auto [rep2, inserted2] = this->insert(method2);
  EXPECT_TRUE(inserted2) << "Expected method2 inserted as new unique code";
  EXPECT_EQ(rep2, method2) << "Expected representative equals inserted method";

  EXPECT_THAT(*this, SizeIs(4u));
}

TEST_F(UniqueMethodTrackerTest, UniqueMethodTrackerNoCode) {
  // Methods without code should return (nullptr, false).
  auto* method = DexMethod::make_method("LFoo;.noCode:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  const auto [rep, inserted] = this->insert(method);
  EXPECT_FALSE(inserted);
  EXPECT_THAT(rep, IsNull());

  // Tracker size should remain unchanged.
  EXPECT_THAT(*this, SizeIs(2u));
}

TEST_F(UniqueMethodTrackerTest, UniqueMethodTrackerNoCfg) {
  // Methods with code but no CFG built should return (nullptr, false).
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.noCfg:()V"
      (
        (return-void)
      )
    )
  )");
  // Deliberately not calling build_cfg().

  const auto [rep, inserted] = this->insert(method);
  EXPECT_FALSE(inserted);
  EXPECT_THAT(rep, IsNull());

  // Tracker size should remain unchanged.
  EXPECT_THAT(*this, SizeIs(2u));
}

TEST_F(UniqueMethodTrackerTest, UniqueMethodTrackerHashCollision) {
  // Two methods with different code but forced to have the same hash.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.collision1:()I"
      (
        (const v0 1)
        (return v0)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.collision2:()I"
      (
        (const v0 2)
        (return v0)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  // Force same hash for both methods to trigger collision handling.
  constexpr size_t kForcedHash = 42;

  const auto [rep1, inserted1] = this->insert(method1, kForcedHash);
  EXPECT_TRUE(inserted1);
  EXPECT_EQ(rep1, method1);

  // method2 has different code, so it should be inserted despite same hash.
  const auto [rep2, inserted2] = this->insert(method2, kForcedHash);
  EXPECT_TRUE(inserted2)
      << "Different code should be inserted even with same hash";
  EXPECT_EQ(rep2, method2);

  EXPECT_THAT(*this, SizeIs(4u));
}
