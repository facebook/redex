/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "Peephole.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

class PeepholeTestB : public RedexTest {};

TEST_F(PeepholeTestB, StringBuilderInit) {
  ClassCreator creator(DexType::make_type("LFoo;"));
  creator.set_super(known_types::java_lang_Object());
  auto method_1 = DexMethod::make_method("LFoo;.b:()V")
                      ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto original_code_1 = R"(
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
  method_1->set_code(assembler::ircode_from_string(original_code_1));
  creator.add_method(method_1);

  auto method_2 = DexMethod::make_method("LFoo;.c:()V")
                      ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto original_code_2 = R"(
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
  method_2->set_code(assembler::ircode_from_string(original_code_2));
  creator.add_method(method_2);

  auto method_3 = DexMethod::make_method("LFoo;.d:()V")
                      ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto original_code_3 = R"(
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
  method_3->set_code(assembler::ircode_from_string(original_code_3));
  creator.add_method(method_3);

  PeepholePass peephole_pass;
  PassManager manager({&peephole_pass});
  ConfigFiles config(Json::nullValue);
  DexStore store("classes");
  store.add_classes({creator.create()});
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));
  manager.run_passes(stores, config);
  TRACE(PEEPHOLE, 1, "\n\n\n\nfinished\n\n\n");
  auto expected_code_1 = assembler::ircode_from_string(R"(
       (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (const-string "foo")
        (move-result-pseudo-object v1)
        (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
        (return-void)
       )
     )");
  EXPECT_EQ(assembler::to_s_expr(method_1->get_code()),
            assembler::to_s_expr(expected_code_1.get()));
  auto expected_code_2 = assembler::ircode_from_string(R"(
        (
         (new-instance "Ljava/lang/StringBuilder;")
         (move-result-pseudo-object v0)
         (const-string "foo")
         (move-result-pseudo-object v1)
         (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
         (return-void)
        )
      )");

  EXPECT_EQ(assembler::to_s_expr(method_2->get_code()),
            assembler::to_s_expr(expected_code_2.get()));

  auto expected_code_3 = assembler::ircode_from_string(R"(
         (
          (new-instance "Ljava/lang/StringBuilder;")
          (move-result-pseudo-object v0)
          (const-string "foo")
          (move-result-pseudo-object v1)
          (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
          (move-object v2 v0)
          (return-void)
         )
       )");

  EXPECT_EQ(assembler::to_s_expr(method_3->get_code()),
            assembler::to_s_expr(expected_code_3.get()));
}

class PeepholeTestNPE : public RedexTest {
 public:
  ::sparta::s_expr run_peephole_pass(const std::string& code) {
    ClassCreator creator(DexType::make_type("LFoo;"));
    creator.set_super(known_types::java_lang_Object());
    auto method = DexMethod::make_method("LFoo;.b:()V")
                      ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(assembler::ircode_from_string(code));
    creator.add_method(method);

    PeepholePass peephole_pass;
    PassManager manager({&peephole_pass});
    ConfigFiles config(Json::nullValue);
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
};

TEST_F(PeepholeTestNPE, ThrowNPEEmpty) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/NullPointerException;.<init>:()V")
      (throw v0)
     )
    )";
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_code = R"(
     (
      (const v0 0)
      (throw v0)
     )
    )";
  auto expected_s_expr = get_s_expr(expected_code);
  ASSERT_EQ(peepholed_s_expr, expected_s_expr);
}

TEST_F(PeepholeTestNPE, ThrowNPENotEmpty) {
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
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_s_expr = get_s_expr(original_code);
  ASSERT_EQ(peepholed_s_expr, expected_s_expr);
}

TEST_F(PeepholeTestNPE, ThrowNonNPEVerifiable) {
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
  ASSERT_EQ(peepholed_s_expr, expected_s_expr) << peepholed_s_expr.str();
}

TEST_F(PeepholeTestNPE, ThrowNonNPENotVerifiable) {
  auto original_code = R"(
     (
      (new-instance "Ljava/lang/IllegalArgumentException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/NullPointerException;.<init>:()V")
      (throw v0)
     )
    )";
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_s_expr = get_s_expr(original_code);
  ASSERT_EQ(peepholed_s_expr, expected_s_expr);
}

TEST_F(PeepholeTestNPE, ThrowNPEBasicBlock) {
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
  auto peepholed_s_expr = run_peephole_pass(original_code);
  auto expected_s_expr = get_s_expr(original_code);
  ASSERT_EQ(peepholed_s_expr, expected_s_expr);
}
