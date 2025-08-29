/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ConstantPropagationPass.h"
#include "DexUtil.h"
#include "RedexTest.h"

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::Not;

class BitwiseOpsConstantPropagationTest : public RedexIntegrationTest {
 protected:
  void set_root_method(std::string_view full_name) {
    auto* method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method) << "Could not find method " << full_name;
    method->rstate.set_root();
  }
};
namespace {

TEST_F(BitwiseOpsConstantPropagationTest, ExtractGreenInt) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainExtractGreen:(I)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, HasSubstr("8-bit deep green"))
      << "\"8-bit deep green\" branch is optimized out, but should't";
  EXPECT_THAT(code_str, Not(HasSubstr("8-bit light green")))
      << "\"8-bit light green\" should be optimized out, but isn't";
}

TEST_F(BitwiseOpsConstantPropagationTest, ExtractGreenLong) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainExtractGreen:(J)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, HasSubstr("16-bit deep green"))
      << "\"16-bit deep green\" branch is optimized out, but should't";
  EXPECT_THAT(code_str, Not(HasSubstr("16-bit light green")))
      << "\"16-bit light green\" is unexpectedly not optimized out";
}

TEST_F(BitwiseOpsConstantPropagationTest, HasNonRedInt) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainHasNonRed:(I)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, Not(HasSubstr("int onlyLowerRed has non-red")))
      << "\"int onlyLowerRed has non-red\" is unexpectedly not optimized out";
  EXPECT_THAT(code_str, HasSubstr("int onlyLowerRed has no non-red"))
      << "\"int onlyLowerRed has no non-red\" branch is unexpectedly optimized "
         "out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("int onlyLowerAlpha has non-red"),
                    HasSubstr("int onlyLowerAlpha has no non-red")))
      << "either int onlyLowerAlpha branch is unexpectedly optimized out";
}

TEST_F(BitwiseOpsConstantPropagationTest, HasNonRedLong) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainHasNonRed:(J)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, Not(HasSubstr("long ohnlyLowerRed has non-red")))
      << "\"long onlyLowerRed has non-red\" should be optimized out, but isn't";
  EXPECT_THAT(code_str, HasSubstr("long onlyLowerRed has no non-red"))
      << "\"long onlyLowerRed has no non-red\" branch is unexpectedly "
         "optimized out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("long onlyLowerAlpha has non-red"),
                    HasSubstr("long onlyLowerAlpha has no non-red")))
      << "either long onlyLowerAlpha branch is unexpectedly optimized out";
}

TEST_F(BitwiseOpsConstantPropagationTest, InvertInt) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainInvert:(I)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, Not(HasSubstr("int alphaless inverted is zero")))
      << "\"int alphaless inverted is zero\" is unexpectedly not optimized out";
  EXPECT_THAT(code_str, HasSubstr("int alphaless inverted is not zero"))
      << "\"int alphaless inverted is not zero\" branch is unexpectedly "
         "optimized out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("int alphaless inverted is 0xFF"),
                    HasSubstr("int alphaless inverted is not 0xFF")))
      << "either \"int alphaless inverted is/isn't 0xFF branch\" is "
         "unexpectedly optimized out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("int alphaless inverted twice is zero"),
                    HasSubstr("int alphaless inverted twice is not zero")))
      << "either \"int alphaless inverted twice is/isn't zero\" branch is "
         "unexpectedly optimized out";
  EXPECT_THAT(code_str, Not(HasSubstr("int alphaless inverted twice is 0xFF")))
      << "\"int alphaless inverted twice is 0xFF\" is unexpectedly not "
         "optimized out";
  EXPECT_THAT(code_str, HasSubstr("int alphaless inverted twice is not 0xFF"))
      << "\"int alphaless inverted twice is not 0xFF\" branch is unexpectedly "
         "optimized out";
}

TEST_F(BitwiseOpsConstantPropagationTest, InvertLong) {
  static constexpr std::string_view root_method_name =
      "LTestRGBA;.mainInvert:(J)V";

  auto scope = build_class_scope(stores);

  set_root_method(root_method_name);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const method = DexMethod::get_method(root_method_name)->as_def();
  auto* const code = method->get_code();
  ASSERT_NE(nullptr, code);
  const auto code_str = assembler::to_string(code);
  EXPECT_THAT(code_str, Not(HasSubstr("long alphaless inverted is zero")))
      << "\"long alphaless inverted is zero\" is unxpectedly not optimized out";
  EXPECT_THAT(code_str, HasSubstr("long alphaless inverted is not zero"))
      << "\"long alphaless inverted is not zero\" branch is unexpectedly "
         "optimized out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("long alphaless inverted is 0xFFFF"),
                    HasSubstr("long alphaless inverted is not 0xFFFF")))
      << "either \"long alphaless inverted is/isn't 0xFFFF branch\" is "
         "unexpectedly optimized out";
  EXPECT_THAT(code_str,
              AllOf(HasSubstr("long alphaless inverted twice is zero"),
                    HasSubstr("long alphaless inverted twice is not zero")))
      << "either \"long alphaless inverted twice is/isn't zero\" branch is "
         "unexpectedly optimized out";
  EXPECT_THAT(code_str,
              Not(HasSubstr("long alphaless inverted twice is 0xFFFF")))
      << "\"long alphaless inverted twice is 0xFFFF\" is unexpectedly not "
         "optimized out";
  EXPECT_THAT(code_str,
              HasSubstr("long alphaless inverted twice is not 0xFFFF"))
      << "\"long alphaless inverted twice is not 0xFFFF\" branch is "
         "unexpectedly optimized out";
}
} // namespace
