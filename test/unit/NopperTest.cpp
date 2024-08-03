/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Nopper.h"
#include "RedexTest.h"
#include "Show.h"

class NopperTest : public RedexTest {
 public:
  NopperTest() {}
};

TEST_F(NopperTest, noppable_blocks_insert_nops) {
  auto code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
    (:L0)
      (return-void)

    (:L1)
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  auto noppable_blocks = nopper_impl::get_noppable_blocks(code->cfg());
  EXPECT_EQ(code->cfg().blocks().size(), 3);
  EXPECT_EQ(noppable_blocks.size(), 3);

  std::unordered_set<cfg::Block*> set(noppable_blocks.begin(),
                                      noppable_blocks.end());
  nopper_impl::insert_nops(code->cfg(), set);
  code->clear_cfg();

  auto expected_str = R"(
    (
      (load-param v0)
      (nop)
      (if-eqz v0 :L1)
    (:L0)
      (nop)
      (return-void)

    (:L1)
      (nop)
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_CODE_EQ(code.get(), expected.get());
}

TEST_F(NopperTest, noppable_blocks_exclusions) {
  auto code_str = R"(
    (
      (load-param v0)
    (:L0)
      (goto :L0)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  auto noppable_blocks = nopper_impl::get_noppable_blocks(code->cfg());
  EXPECT_EQ(code->cfg().blocks().size(), 2);
  EXPECT_EQ(noppable_blocks.size(), 0);
}

TEST_F(NopperTest, noppable_auxiliary_defs) {
  auto nopper_type = DexType::make_type("Lnopper;");
  auto ad = nopper_impl::create_auxiliary_defs(nopper_type);
  ASSERT_NE(ad.cls, nullptr);
  ASSERT_EQ(ad.cls->get_type(), nopper_type);

  ASSERT_NE(ad.int_field, nullptr);
  ASSERT_EQ(show(ad.int_field), "Lnopper;.int_field:I");

  ASSERT_NE(ad.clinit, nullptr);
  ASSERT_EQ(show(ad.clinit), "Lnopper;.<clinit>:()V");
  auto expected_str = R"(
    (
      (.pos:dbg_0 "Lnopper;.<clinit>:()V" RedexGenerated 0) 
      (const v0 10) 
      (invoke-static (v0) "Lnopper;.fib:(I)I")
      (move-result v0) 
      (sput v0 "Lnopper;.int_field:I") 
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);
  ad.clinit->get_code()->clear_cfg();
  EXPECT_CODE_EQ(ad.clinit->get_code(), expected.get());

  ASSERT_NE(ad.fib_method, nullptr);
  ASSERT_EQ(show(ad.fib_method), "Lnopper;.fib:(I)I");
  expected_str = R"(
    (
      (load-param v2) 
      (.pos:dbg_0 "Lnopper;.fib:(I)I" RedexGenerated 0) 
      (const v0 1) 
      (if-gt v2 v0 :L0) 
      (return v2) 
    (:L0) 
      (add-int/lit v0 v0 -1) 
      (invoke-static (v0) "Lnopper;.fib:(I)I") 
      (move-result v1) 
      (add-int/lit v0 v0 -1) 
      (invoke-static (v0) "Lnopper;.fib:(I)I") 
      (move-result v0) 
      (add-int v0 v0 v1) 
      (return v0) 
    )
  )";
  expected = assembler::ircode_from_string(expected_str);
  ad.fib_method->get_code()->clear_cfg();
  EXPECT_CODE_EQ(ad.fib_method->get_code(), expected.get());
}

TEST_F(NopperTest, noppable_blocks_insert_nops_with_auxiliary_defs) {
  auto code_str = R"(
    (
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  auto noppable_blocks = nopper_impl::get_noppable_blocks(code->cfg());
  EXPECT_EQ(code->cfg().blocks().size(), 1);
  EXPECT_EQ(noppable_blocks.size(), 1);

  std::unordered_set<cfg::Block*> set(noppable_blocks.begin(),
                                      noppable_blocks.end());
  auto nopper_type = DexType::make_type("Lnopper;");
  auto ad = nopper_impl::create_auxiliary_defs(nopper_type);
  nopper_impl::insert_nops(code->cfg(), set, &ad);
  code->clear_cfg();

  auto expected_str = R"(
    (
      (const v0 4) 
      (invoke-static (v0) "Lnopper;.fib:(I)I") 
      (move-result v0)
      (add-int/lit v0 v0 27) 
      (mul-int/lit v0 v0 77) 
      (add-int/lit v0 v0 27) 
      (mul-int/lit v0 v0 77) 
      (add-int/lit v0 v0 27) 
      (mul-int/lit v0 v0 77) 
      (add-int/lit v0 v0 27) 
      (mul-int/lit v0 v0 77) 
      (sput v0 "Lnopper;.int_field:I")
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_CODE_EQ(code.get(), expected.get());
}
