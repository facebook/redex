/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DedupBlocks.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "TypeInference.h"

#include <string>

struct TypeInferenceTest : public RedexIntegrationTest {
  DexType* m_special_exception_type;

  DexMethodRef* m_what_is_this;

  void SetUp() override {
    m_special_exception_type =
        DexType::get_type("Lcom/facebook/redextest/MySpecialException;");
    always_assert(m_special_exception_type);

    m_what_is_this = DexMethod::get_method(
        "Lcom/facebook/redextest/TypeInferenceTest;.whatIsThisThrowable:(Ljava/"
        "lang/Throwable;)V");
    always_assert(m_what_is_this);
  }
};

TEST_F(TypeInferenceTest, test_move_exception_type) {
  auto scope = build_class_scope(stores);

  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypeInferenceTest;.testExceptionTypeInference:()V")
                    ->as_def();

  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(method);

  bool insn_found = false;
  auto& envs = inference.get_type_environments();
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && insn->get_method() == m_what_is_this) {
      auto& env = envs.at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      ASSERT_TRUE(dex_type);
      EXPECT_EQ(m_special_exception_type, *dex_type);
      insn_found = true;
    }
  }

  // Do not fail silently.
  EXPECT_TRUE(insn_found);
}

TEST_F(TypeInferenceTest, test_dedup_blocks_exception_type) {
  auto scope = build_class_scope(stores);

  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypeInferenceTest;.testCatch2Types:()V")
                    ->as_def();

  auto code = method->get_code();
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  using namespace dedup_blocks_impl;

  Config empty_config;
  DedupBlocks db(empty_config, method);
  db.run();

  type_inference::TypeInference inference(cfg);
  inference.run(method);

  int insn_found = 0;
  auto& envs = inference.get_type_environments();
  for (auto& mie : InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && insn->get_method() == m_what_is_this) {
      auto& env = envs.at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      ASSERT_TRUE(dex_type);
      EXPECT_EQ(m_special_exception_type, *dex_type);
      insn_found++;
    }
  }

  // Do not fail silently.
  EXPECT_EQ(1, insn_found);
}
