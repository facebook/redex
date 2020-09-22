/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BranchPrefixHoisting.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "Show.h"

class BranchPrefixHoistingTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_instructions_hoisted) {

  DexType* type = DexType::make_type("testClass");
  auto cls = create_class(type, type::java_lang_Object(), {}, ACC_PUBLIC);
  auto args = DexTypeList::make_type_list({type::_int()});
  auto proto = DexProto::make_proto(type::_void(), args);
  DexMethod* method =
      DexMethod::make_method(type, DexString::make_string("test"), proto)
          ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  cls->add_method(method);
  method->set_code(assembler::ircode_from_string(code_str));
  auto code = method->get_code();
  code->build_cfg(true);
  auto& cfg = code->cfg();
  std::cerr << "before:" << std::endl << SHOW(cfg);

  type_inference::TypeInference type_inference(cfg);
  type_inference.run(method);
  int actual_insns_hoisted =
      BranchPrefixHoistingPass::process_cfg(cfg, type_inference);

  std::cerr << "after:" << std::endl << SHOW(code->cfg());
  EXPECT_EQ(expected_instructions_hoisted, actual_insns_hoisted);
  auto expected = assembler::ircode_from_string(expected_str);
  auto expected_ptr = expected.get();
  expected_ptr->build_cfg();
  auto& expected_cfg = expected_ptr->cfg();
  std::cerr << "expected:" << std::endl << SHOW(expected_cfg);

  EXPECT_EQ(assembler::to_s_expr(code), assembler::to_s_expr(expected.get()));
}

TEST_F(BranchPrefixHoistingTest, simple_insn_hoisting) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 6)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (if-eqz v0 :true)
      (const v6 6)
      (goto :end)
      (:true)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 5);
}

TEST_F(BranchPrefixHoistingTest, stop_hoisting_at_side_effect) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v0 7)
      (const v2 3)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v0 7)
      (const v2 4)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (move v4 v0)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v0 7)
      (if-eqz v4 :true)
      (const v2 3)
      (goto :end)
      (:true)
      (const v2 4)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 4);
}

TEST_F(BranchPrefixHoistingTest, move_result_hoist_ok) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (if-eqz v0 :true)
      (const v5 42)
      (goto :end)
      (:true)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, move_result_no_hoist_diff_dest) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v4)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v4)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}

TEST_F(BranchPrefixHoistingTest, move_result_no_hoist_on_side_effect) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, one_block_becomes_empty) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (if-eqz v0 :true)
      (goto :end)
      (:true)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, both_blocks_becomes_empty) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (if-eqz v0 :true)
      (goto :end)
      (:true)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 3);
}

TEST_F(BranchPrefixHoistingTest, move_result_wide) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (const-wide v1 2)
      (const-wide v2 10)
      (if-ge v3 v0 :true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (goto :end)
      (:true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const-wide v1 2)
      (const-wide v2 10)
      (if-ge v3 v0 :true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (goto :end)
      (:true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, branch_goes_to_same_block) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (if-eqz v0 :true)
      (:true)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 6);
}

TEST_F(BranchPrefixHoistingTest, switch_two_same_cases) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (switch v0 (:case1 :case2))
      (:case1 1)
      (const v1 1)
      (const v2 2)
      (goto :end)
      (:case2 2)
      (const v1 1)
      (const v2 2)
      (goto :end)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (switch v0 (:case1 :case2))
      (:case1 1)
      (goto :end)
      (:case2 2)
      (goto :end)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, switch_with_same_cases) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (switch v0 (:a :b :c :d :e :f))

      (:a 0)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v2 v1)
      (goto :end)

      (:b 1)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v2 v1)
      (goto :end)

      (:c 2)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v2 v1)
      (goto :end)

      (:d 3)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v1 v1)
      (goto :end)

      (:e 4)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v1 v1)
      (goto :end)

      (:f 5)
      (const v1 1)
      (add-int v2 v1 v1)
      (add-int v2 v1 v1)
      (goto :end)

      (:end)
      (return-void)
    )
  )";

  auto expected_str = R"(
     (
      (load-param v0)
      (const v1 1)
      (add-int v2 v1 v1)
      (switch v0 (:a :b :c :d :e :f))

      (:a 0)
      (add-int v2 v2 v1)
      (goto :end)

      (:b 1)
      (add-int v2 v2 v1)
      (goto :end)

      (:c 2)
      (add-int v2 v2 v1)
      (goto :end)

      (:d 3)
      (add-int v2 v1 v1)
      (goto :end)

      (:e 4)
      (add-int v2 v1 v1)
      (goto :end)

      (:f 5)
      (add-int v2 v1 v1)
      (goto :end)

      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, branch_with_same_return) {
  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (return-void)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (return-void)
    )
  )";

  const auto& expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (return-void)
      (if-eqz v0 :true)
      (:true)
    )
  )";
  test(code_str, expected_str, 4);
}

TEST_F(BranchPrefixHoistingTest, branch_with_clber_wide) {

  const auto& code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (const-wide v0 1)
      (add-int v2 v0 v1)
      (add-int v2 v1 v1)
      (goto :end)
      (:true)
      (const-wide v0 1)
      (add-int v2 v0 v1)
      (add-int v2 v2 v1)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (move v3 v0)
      (const-wide v0 1)
      (add-int v2 v1 v1)
      (if-eqz v3 :true)
      (add-int v2 v1 v1)
      (goto :end)
      (:true)
      (add-int v2 v2 v1)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}
