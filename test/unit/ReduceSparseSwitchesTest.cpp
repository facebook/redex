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

TEST_F(ReduceSparseSwitchesTest, trivial_switch_case) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2))

        (:L1 50)
        (return-void)

        (:L0 0) 
        (return-void)

        (:L2 100) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::trivial_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.removed_trivial_switch_cases, 1);
  EXPECT_EQ(stats.removed_trivial_switches, 0);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (switch v0 (:L0 :L1)) 
      (return-void) 
      (:L0 0)
      (return-void) 
      (:L1 100) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, trivial_switch) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0))

        (:L0 50)
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::trivial_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.removed_trivial_switch_cases, 1);
  EXPECT_EQ(stats.removed_trivial_switches, 1);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (return-void) 
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

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
      4, 2, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_packed_segments, 1);
  EXPECT_EQ(stats.splitting_transformations_switch_cases_packed, 2);

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

TEST_F(ReduceSparseSwitchesTest, splittingEvenSizeSwitch2) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3))

        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 1) 
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
      4, 2, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_packed_segments, 2);
  EXPECT_EQ(stats.splitting_transformations_switch_cases_packed, 4);

  const auto& expected_str = R"(
    (
      (load-param v0)
      (switch v0 (:L2 :L3))

      (switch v0 (:L0 :L1))
      (return-void)

      (:L0 100) 
      (return-void)
      (:L1 101) 
      (return-void)

      (:L2 0) 
      (return-void)
      (:L3 1) 
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
      5, 2, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_packed_segments, 1);
  EXPECT_EQ(stats.splitting_transformations_switch_cases_packed, 3);

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

TEST_F(ReduceSparseSwitchesTest, splittingPerfectly) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3))

        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 1) 
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
      4, 2, method->get_code()->cfg());
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_packed_segments, 2);
  EXPECT_EQ(stats.splitting_transformations_switch_cases_packed, 4);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (switch v0 (:L2 :L3)) 
      (switch v0 (:L0 :L1)) 
      (return-void) 
      
      (:L0 100) 
      (return-void) 
      (:L1 101) 
      (return-void) 
      
      (:L2 0) 
      (return-void) 
      (:L3 1) 
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

TEST_F(ReduceSparseSwitchesTest, splittingIntoLog2ManyChunks) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)
        (switch v0 (:L0 :L1 :L2 :L3 :L4 :L5 :L6 :L7 :L8 :L9 :L10 :L11 :L12 :L13 :L14 :L15))

        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 1) 
        (return-void)
        (:L2 2) 
        (return-void)
        (:L3 3) 
        (return-void)
        (:L4 4) 
        (return-void)
        (:L5 5) 
        (return-void)
        (:L6 6) 
        (return-void)

        (:L7 50) 
        (return-void)

        (:L8 100) 
        (return-void)
        (:L9 101) 
        (return-void)
        (:L10 102) 
        (return-void)
        (:L11 103) 
        (return-void)
        (:L12 104) 
        (return-void)
        (:L13 105) 
        (return-void)
        (:L14 106) 
        (return-void)

        (:L15 150) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::splitting_transformation(
      10, 3, method->get_code()->cfg());
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.splitting_transformations, 1);
  EXPECT_EQ(stats.splitting_transformations_packed_segments, 2);
  EXPECT_EQ(stats.splitting_transformations_switch_cases_packed, 14);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (switch v0 (:L9 :L10 :L11 :L12 :L13 :L14 :L15)) 
      (switch v0 (:L2 :L3 :L4 :L5 :L6 :L7 :L8)) 
      (switch v0 (:L0 :L1)) 
      (return-void) 

      (:L0 150) 
      (return-void) 
      (:L1 50) 
      (return-void) 
      
      (:L2 106) 
      (return-void) 
      (:L3 105) 
      (return-void) 
      (:L4 104) 
      (return-void) 
      (:L5 103) 
      (return-void) 
      (:L6 102) 
      (return-void) 
      (:L7 101) 
      (return-void) 
      (:L8 100) 
      (return-void) 
      
      (:L9 6) 
      (return-void) 
      (:L10 5) 
      (return-void) 
      (:L11 4) 
      (return-void) 
      (:L12 3) 
      (return-void) 
      (:L13 2) 
      (return-void) 
      (:L14 1) 
      (return-void) 
      (:L15 0) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, expand_sparse) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)

        (switch v0 (:L0 :L1 :L2))
        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 11) 
        (return-void)
        (:L2 222) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::expand_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.expanded_transformations, 1);
  EXPECT_EQ(stats.expanded_switch_cases, 3);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (if-eqz v0 :L2) 
      (const v1 11) 
      (if-eq v0 v1 :L1) 
      (const v1 222) 
      (if-eq v0 v1 :L0) 
      (return-void) 
      (:L0) 
      (return-void) 
      (:L1) 
      (return-void) 
      (:L2) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, expand_very_small_packed) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)

        (switch v0 (:L0 :L1 :L2))
        (return-void)

        (:L0 0) 
        (return-void)
        (:L1 1) 
        (return-void)
        (:L2 2) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::expand_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.expanded_transformations, 1);
  EXPECT_EQ(stats.expanded_switch_cases, 3);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (if-eqz v0 :L2) 
      (const v1 1) 
      (if-eq v0 v1 :L1) 
      (const v1 2) 
      (if-eq v0 v1 :L0) 
      (return-void) 
      (:L0) 
      (return-void) 
      (:L1) 
      (return-void) 
      (:L2) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, expand_add) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)

        (switch v0 (:L0 :L1 :L2))
        (return-void)

        (:L0 10000000) 
        (return-void)
        (:L1 10000011) 
        (return-void)
        (:L2 10000022) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::expand_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.expanded_transformations, 1);
  EXPECT_EQ(stats.expanded_switch_cases, 3);

  const auto& expected_str = R"(
    (
      (load-param v0) 
      (const v1 10000000) 
      (if-eq v0 v1 :L2) 
      (add-int/lit v1 v1 11) 
      (if-eq v0 v1 :L1) 
      (add-int/lit v1 v1 11) 
      (if-eq v0 v1 :L0) 
      (return-void) 
      (:L0) 
      (return-void) 
      (:L1) 
      (return-void) 
      (:L2) 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());
}

TEST_F(ReduceSparseSwitchesTest, expand_not) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")LtestClass;.testMethod(:(I)V"
      (
        (load-param v0)

        (switch v0 (:L0 :L1 :L2 :L3 :L4))
        (return-void)

        (:L0 10000000) 
        (return-void)
        (:L1 10000011) 
        (return-void)
        (:L2 10000022) 
        (return-void)
        (:L3 10000023) 
        (return-void)
        (:L4 10000024) 
        (return-void)
      )
    )
  )");
  method->get_code()->build_cfg();

  auto stats = ReduceSparseSwitchesPass::expand_transformation(
      method->get_code()->cfg());
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.expanded_transformations, 0);
  EXPECT_EQ(stats.expanded_switch_cases, 0);
}
