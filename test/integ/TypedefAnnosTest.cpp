/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gtest/gtest.h"
#include <string>

#include "DexClass.h"
#include "RedexTest.h"
#include "TypeInference.h"

struct TypedefAnnosTest : public RedexIntegrationTest {
  cfg::ControlFlowGraph& get_cfg(DexMethod* method) {
    auto code = method->get_code();
    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();
    return cfg;
  }
  std::unordered_set<DexType*> get_annotation_set() {
    std::unordered_set<DexType*> anno_set;
    anno_set.emplace(
        DexType::make_type("Lcom/facebook/redex/annotations/SafeIntDef;"));
    anno_set.emplace(
        DexType::make_type("Lcom/facebook/redex/annotations/SafeStringDef;"));
    return anno_set;
  }
};

TEST_F(TypedefAnnosTest, test_anno_load_param_object) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testAnnoObject:(Lcom/facebook/redextest/"
                    "I;)Lcom/facebook/redextest/I;")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestIntDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_int_anno_load_param) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testIntAnnoParam:(I)I")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestIntDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_anno_invoke_static) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testAnnoInvokeStatic:(Lcom/facebook/"
                    "redextest/"
                    "I;)Lcom/facebook/redextest/I;")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestIntDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_int_anno_invoke_static) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testIntAnnoInvokeStatic:(I)I")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestIntDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_string_anno_load_parm) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testStringAnnoParam:(Ljava/lang/"
                    "String;)Ljava/lang/String;")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestStringDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_string_anno_invoke_static) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testStringAnnoInvokeStatic:(Ljava/lang/"
                    "String;)Ljava/lang/String;")
                    ->as_def();
  auto& cfg = get_cfg(method);
  const auto& val = get_annotation_set();
  type_inference::TypeInference inference(cfg, false, val);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_EQ(*(exit_env.get_annotation(insn->src(0))),
              DexType::get_type("Linteg/TestStringDef;"));
  }
}

TEST_F(TypedefAnnosTest, test_no_anno_inference) {
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnosTest;.testAnnoObject:(Lcom/facebook/redextest/"
                    "I;)Lcom/facebook/redextest/I;")
                    ->as_def();
  auto& cfg = get_cfg(method);

  type_inference::TypeInference inference(cfg, false);
  inference.run(method);

  for (auto block : cfg.real_exit_blocks()) {
    IRInstruction* insn = block->get_last_insn()->insn;
    const auto& exit_env = inference.get_exit_state_at(block);
    EXPECT_FALSE(exit_env.get_annotation(insn->src(0)));
  }
}
