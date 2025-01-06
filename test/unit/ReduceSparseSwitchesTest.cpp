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

  auto stats = ReduceSparseSwitchesPass::splitting_transformation(4, method);
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

  auto stats = ReduceSparseSwitchesPass::splitting_transformation(5, method);
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

TEST_F(ReduceSparseSwitchesTest, binarySearch) {
  ClassCreator cc(DexType::make_type("LtestClass;"));
  cc.set_super(type::java_lang_Object());
  auto* cls = cc.create();
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

  size_t running_index{42};
  size_t method_refs{0};
  size_t field_refs{0};
  InsertOnlyConcurrentMap<DexMethod*, DexMethod*> init_methods;

  auto stats = ReduceSparseSwitchesPass::binary_search_transformation(
      4, cls, method, running_index, method_refs, field_refs, &init_methods);
  method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  method->get_code()->build_cfg();
  method->get_code()->clear_cfg();

  EXPECT_EQ(stats.binary_search_transformations, 1);
  EXPECT_EQ(stats.binary_search_transformations_switch_cases, 6);

  auto* array_type = DexType::make_type("[I");
  // Static array holder field got created
  auto* field = cls->find_sfield("$sparse_index$42", array_type);
  EXPECT_NE(field, nullptr);
  EXPECT_TRUE(is_volatile(field));

  const auto& expected_str = R"(
    (
      (load-param v0)
      (sget-object "LtestClass;.$sparse_index$42:[I")
      (move-result-pseudo-object v1)
      (if-nez v1 :initialized)
      (invoke-static () "LtestClass;.$sparse_index_init$42:()[I")
      (move-result-object v1)
      (:initialized)
      (invoke-static (v1 v0) "Ljava/util/Arrays;.binarySearch:([II)I")
      (move-result v1)
      (switch v1 (:L0 :L1 :L2 :L3 :L4))
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
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), method->get_code());

  auto* init_method =
      cls->find_method_from_simple_deobfuscated_name("$sparse_index_init$42");
  EXPECT_NE(init_method, nullptr);
  ASSERT_EQ(init_methods.size(), 1);
  ASSERT_EQ(init_methods.count(init_method), 1);
  EXPECT_EQ(init_methods.at(init_method), method);
  EXPECT_EQ(running_index, 42 + 1);
  EXPECT_EQ(method_refs, 1); // init method
  EXPECT_EQ(field_refs, 1); // array holder field

  init_method->get_code()->clear_cfg();
  // Rebuild an extra time to work around an ordering quirk in switch cases.
  const auto& expected_init_str = R"(
    (
      (const v0 5) 
      (new-array v0 "[I") 
      (move-result-pseudo-object v0) 
      (fill-array-data v0 #4 (0 32 64 66 68)) 
      (sput-object v0 "LtestClass;.$sparse_index$42:[I") 
      (return-object v0)
    )
  )";
  auto expected_init = assembler::ircode_from_string(expected_init_str);
  EXPECT_CODE_EQ(expected_init.get(), init_method->get_code());
}
