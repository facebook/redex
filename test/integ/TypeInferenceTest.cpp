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

  cfg::ControlFlowGraph& get_cfg(DexMethod* method) {
    auto code = method->get_code();
    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();
    return cfg;
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
  DedupBlocks db(&empty_config, method);
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

TEST_F(TypeInferenceTest, test_join_with_null) {
  auto scope = build_class_scope(stores);
  auto method1 = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "TypeInferenceTest;.testJoinWithNull1:()Lcom/facebook/"
                     "redextest/Base;")
                     ->as_def();
  auto& cfg1 = get_cfg(method1);

  type_inference::TypeInference inference1(cfg1);
  inference1.run(method1);
  auto exit_block = cfg1.exit_block();
  auto exit_env = inference1.get_exit_state_at(exit_block);

  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_return(insn->opcode())) {
      continue;
    }
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_EQ(*ret_type.get_dex_type(),
              DexType::get_type("Lcom/facebook/redextest/Base;"));
    EXPECT_TRUE(ret_type.is_nullable());
  }

  auto method2 = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "TypeInferenceTest;.testJoinWithNull2:()Lcom/facebook/"
                     "redextest/Base;")
                     ->as_def();
  auto& cfg2 = get_cfg(method2);

  type_inference::TypeInference inference2(cfg2);
  inference2.run(method2);
  exit_block = cfg2.exit_block();
  exit_env = inference2.get_exit_state_at(exit_block);

  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_return(insn->opcode())) {
      continue;
    }
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_EQ(*ret_type.get_dex_type(),
              DexType::get_type("Lcom/facebook/redextest/Base;"));
    EXPECT_TRUE(ret_type.is_nullable());
  }

  auto method3 = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "TypeInferenceTest;.testJoinWithNull3:()Lcom/facebook/"
                     "redextest/Base;")
                     ->as_def();
  auto& cfg3 = get_cfg(method3);

  type_inference::TypeInference inference3(cfg3);
  inference3.run(method3);
  exit_block = cfg3.exit_block();
  exit_env = inference3.get_exit_state_at(exit_block);

  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_return(insn->opcode())) {
      continue;
    }
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_FALSE(ret_type.get_dex_type());
    EXPECT_TRUE(ret_type.is_null());
  }

  auto method4 = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "TypeInferenceTest;.testJoinWithNull4:()I")
                     ->as_def();
  auto& cfg4 = get_cfg(method4);

  type_inference::TypeInference inference4(cfg4);
  inference4.run(method4);
  exit_block = cfg4.exit_block();
  exit_env = inference4.get_exit_state_at(exit_block);

  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_return(insn->opcode())) {
      continue;
    }
    EXPECT_EQ(exit_env.get_type(insn->src(0)), type_inference::TypeDomain(INT));
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_TRUE(ret_type.is_top());
  }
}

TEST_F(TypeInferenceTest, test_small_set_domain) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypeInferenceTest;.testSmallSetDomain:()V")
                    ->as_def();
  auto& cfg = get_cfg(method);

  type_inference::TypeInference inference(cfg);
  inference.run(method);

  auto exit_block = cfg.exit_block();
  auto exit_env = inference.get_exit_state_at(exit_block);
  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_invoke(insn->opcode())) {
      continue;
    }
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_TRUE(ret_type.get_dex_type());
    EXPECT_EQ(*ret_type.get_dex_type(),
              DexType::get_type("Lcom/facebook/redextest/Base;"));
    auto type_set = ret_type.get_type_set();
    EXPECT_EQ(type_set.size(), 2);
    EXPECT_TRUE(
        type_set.contains(DexType::get_type("Lcom/facebook/redextest/Sub1;")));
    EXPECT_TRUE(
        type_set.contains(DexType::get_type("Lcom/facebook/redextest/Sub2;")));
  }
}

TEST_F(TypeInferenceTest, test_join_with_interface) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypeInferenceTest;.testJoinWithInterface:()V")
                    ->as_def();
  auto& cfg = get_cfg(method);

  type_inference::TypeInference inference(cfg);
  inference.run(method);
  auto exit_block = cfg.exit_block();
  auto exit_env = inference.get_exit_state_at(exit_block);
  for (auto& mie : InstructionIterable(exit_block)) {
    auto insn = mie.insn;
    if (!is_invoke(insn->opcode())) {
      continue;
    }
    auto ret_type = exit_env.get_type_domain(insn->src(0));
    EXPECT_TRUE(ret_type.get_dex_type());
    auto type_set = ret_type.get_type_set();
    EXPECT_EQ(*ret_type.get_dex_type(),
              DexType::get_type("Lcom/facebook/redextest/I;"));
    EXPECT_EQ(type_set.size(), 2);
    EXPECT_TRUE(
        type_set.contains(DexType::get_type("Lcom/facebook/redextest/I;")));
    EXPECT_TRUE(
        type_set.contains(DexType::get_type("Lcom/facebook/redextest/C;")));
  }
}
