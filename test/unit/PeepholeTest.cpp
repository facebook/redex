/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <sstream>

#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "Peephole.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

class PeepholeTest : public RedexTest {
 public:
  ::sparta::s_expr run_peephole_pass(const std::string& code) {
    auto class_name = next_class();
    ClassCreator creator(DexType::make_type(class_name));
    creator.set_super(type::java_lang_Object());
    auto signature = class_name + ".foo:()V";
    auto method = DexMethod::make_method(signature)->make_concrete(
        ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(assembler::ircode_from_string(code));
    creator.add_method(method);

    PeepholePass peephole_pass;
    PassManager manager({&peephole_pass});
    ConfigFiles config(Json::nullValue);
    config.parse_global_config();
    DexStore store("classes");
    store.add_classes({creator.create()});
    std::vector<DexStore> stores;
    stores.emplace_back(std::move(store));
    manager.run_passes(stores, config);

    return assembler::to_s_expr(method->get_code());
  }

  ::sparta::s_expr get_s_expr(const std::string& code) {
    return assembler::to_s_expr(assembler::ircode_from_string(code).get());
  }

  ::testing::AssertionResult run_test(const std::string& input,
                                      const std::string& expected) {
    auto peepholed_s_expr = run_peephole_pass(input);
    auto expected_s_expr = get_s_expr(expected);
    if (peepholed_s_expr == expected_s_expr) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << input << "\nevaluates to\n"
           << peepholed_s_expr.str() << "\ninstead of\n"
           << expected_s_expr.str();
  }

 private:
  // Generate unique class names, as tests may be run in parallel.
  static std::string next_class() {
    size_t cur = count_.fetch_add(1u);
    std::ostringstream oss;
    oss << "LFoo" << cur << ";";
    return oss.str();
  }

  static std::atomic<size_t> count_;
};

std::atomic<size_t> PeepholeTest::count_{0};

class PeepholeStringBuilderTest : public PeepholeTest {};

TEST_F(PeepholeStringBuilderTest, ReduceEmptyInitMoveResultSame) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(PeepholeStringBuilderTest, ReduceEmptyInitNoMoveResult) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(PeepholeStringBuilderTest, ReduceEmptyInitMoveResultOther) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v2)
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
      (move-object v2 v0)
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

class PeepholeNPETest : public PeepholeTest {};

TEST_F(PeepholeNPETest, ThrowNPEEmpty) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/NullPointerException;.<init>:()V")
      (throw v0)
     )
    )";
  auto expected_code = R"(
     (
      (const v0 0)
      (throw v0)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(PeepholeNPETest, ThrowNPENotEmpty) {
  auto original_code = R"(
     (
      (const-string "Test")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (throw v0)
     )
    )";
  ASSERT_TRUE(run_test(original_code, original_code));
}

TEST_F(PeepholeNPETest, ThrowNonNPEVerifiable) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/IllegalArgumentException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/IllegalArgumentException;.<init>:()V")
      (throw v0)
     )
    )";
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_s_expr = get_s_expr(original_code);
  ASSERT_TRUE(run_test(original_code, original_code));
}

TEST_F(PeepholeNPETest, ThrowNonNPENotVerifiable) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/IllegalArgumentException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/IllegalArgumentException;.<init>:()V")
      (throw v0)
     )
    )";
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_s_expr = get_s_expr(original_code);
  ASSERT_TRUE(run_test(original_code, original_code));
}

TEST_F(PeepholeNPETest, ThrowNPEBasicBlock) {
  auto original_code = R"(
     (
      (const v1 0)
      (if-eqz v1 :other_exception)
      (const-string "Test")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (goto :the_throw)
      (:other_exception)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/NullPointerException;.<init>:()V")
      (:the_throw)
      (throw v0)
     )
    )";
  auto original_code_reordered = R"(
     (
      (const v1 0)
      (if-eqz v1 :other_exception)
      (const-string "Test")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (:the_throw)
      (throw v0)
      (:other_exception)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/NullPointerException;.<init>:()V")
      (goto :the_throw)
     )
    )";
  ASSERT_TRUE(run_test(original_code, original_code_reordered));
}
