/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "RedexTest.h"
#include "ReduceSparseSwitchesPass.h"

struct ReduceSparseSwitchesTest : public RedexTest {};

TEST_F(ReduceSparseSwitchesTest, splittingEvenSizeSwitch) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3))

        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 50) 
        (return-void)

        (:L2 100) 
        (return-void)
        (:L3 101) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::splitting_transformation(
      4, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_switch_cases, 2);

  const auto& expected_str = R"(
    (
      (load-param v0)
      (switch v0 (:L2 :L3))

      (switch v0 (:L0 :L1))
      (return-void)

      (:L0 0) 
      (return-void)
      (:L1 50) 
      (return-void)

      (:L2 100) 
      (return-void)
      (:L3 101) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, splittingOddSizeSwitch) {
  // Note that we considered as for a "packed" switch any switch that is not
  // "sufficiently sparse". Or in other words: It's good enough for a packed
  // switch if at most half of the case keys in its extent are missing.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3 :L4))

        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 50) 
        (return-void)

        (:L2 100) 
        (return-void)
        (:L3 102) 
        (return-void)
        (:L4 104) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::splitting_transformation(
      5, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_switch_cases, 3);

  const auto& expected_str = R"(
    (
      (load-param v0)
      (switch v0 (:L2 :L3 :L4))

      (switch v0 (:L0 :L1))
      (return-void)

      (:L0 0) 
      (return-void)
      (:L1 50) 
      (return-void)

      (:L2 100) 
      (return-void)
      (:L3 102) 
      (return-void)
      (:L4 104) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, multiplexing) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3 :L4))
        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 3) 
        (return-void)
        (:L2 6) 
        (return-void)
        (:L3 9) 
        (return-void)
        (:L4 12) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::multiplexing_transformation(
      5, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.multiplexing.size(), 1);
  EXPECT_EQ(stats.multiplexing.begin()->first, 4);
  auto& mstats = stats.multiplexing.begin()->second;
  EXPECT_EQ(mstats.abandoned, 0);
  EXPECT_EQ(mstats.transformations, 1);
  EXPECT_EQ(mstats.switch_cases, 5);
  EXPECT_EQ(mstats.inefficiency, 0);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (and-int/lit v1 v0 3) 
      (switch v1 (:L1 :L2 :L3 :L4)) 
    (:L0) 
      (return-void) 
    (:L1 0) 
      (switch v0 (:L5 :L6)) 
      (goto :L0) 
    (:L2 1) 
      (const v1 9) 
      (if-ne v0 v1 :L0)
      (return-void) 
    (:L3 2) 
      (const v1 6) 
      (if-ne v0 v1 :L0) 
      (return-void) 
    (:L4 3) 
      (const v1 3) 
      (if-ne v0 v1 :L0) 
      (return-void) 
    (:L5 0) 
      (return-void) 
    (:L6 12) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, multiplexing_shr) {
  // Almost same situation as in multiplexing test, but now all the case keys
  // are doubled, and here our algorithm figures out that we should first shift
  // the selector by 1 to get the best distribution.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3 :L4))
        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 6) 
        (return-void)
        (:L2 12) 
        (return-void)
        (:L3 18) 
        (return-void)
        (:L4 24) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::multiplexing_transformation(
      5, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.multiplexing.size(), 1);
  EXPECT_EQ(stats.multiplexing.begin()->first, 4);
  auto& mstats = stats.multiplexing.begin()->second;
  EXPECT_EQ(mstats.abandoned, 0);
  EXPECT_EQ(mstats.transformations, 1);
  EXPECT_EQ(mstats.switch_cases, 5);
  EXPECT_EQ(mstats.inefficiency, 0);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (shr-int/lit v1 v0 1)
      (and-int/lit v1 v1 3) 
      (switch v1 (:L1 :L2 :L3 :L4)) 
    (:L0) 
      (return-void) 
    (:L1 0) 
      (switch v0 (:L5 :L6)) 
      (goto :L0) 
    (:L2 1) 
      (const v1 18) 
      (if-ne v0 v1 :L0)
      (return-void) 
    (:L3 2) 
      (const v1 12) 
      (if-ne v0 v1 :L0) 
      (return-void) 
    (:L4 3) 
      (const v1 6)
      (if-ne v0 v1 :L0) 
      (return-void) 
    (:L5 0) 
      (return-void) 
    (:L6 24) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}
