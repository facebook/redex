/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ABExperimentContextImpl.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "RedexTest.h"

namespace ab_test {

struct ABExperimentContextTest : public RedexIntegrationTest {
  void SetUp() override { ABExperimentContextImpl::reset_global_state(); }
};

void change_called_method(const std::string& exp_name,
                          DexMethod* m,
                          const std::string& original_method_name,
                          const std::string& new_method_name) {
  auto& cfg = m->get_code()->cfg();
  ab_test::ABExperimentContextImpl experiment(&cfg, m, exp_name);

  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    IRInstruction* insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {

      auto m_ref = insn->get_method();
      if (m_ref->get_name()->str() == original_method_name) {
        DexString* name = DexString::make_string(new_method_name);
        DexMethodRef* method = DexMethod::make_method(m_ref->get_class(), name,
                                                      m_ref->get_proto());
        insn->set_method(method);
        break;
      }
    }
  }
}

TEST_F(ABExperimentContextTest, testCFGConstructorBasicFunctionality) {
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("basicMethod");
  ASSERT_TRUE(m != nullptr);

  m->get_code()->build_cfg(/* editable */ true);

  ab_test::ABExperimentContextImpl experiment(&m->get_code()->cfg(), m,
                                              "ab_experiment");
  experiment.flush();
  ASSERT_TRUE(!m->get_code()->cfg_built());
}

TEST_F(ABExperimentContextTest, testTestingMode) {
  ab_test::ABExperimentContextImpl::parse_experiments_states(
      {{"ab_experiment", "test"}});

  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);
  m->get_code()->build_cfg(true);
  change_called_method("ab_experiment", m, "getSixPrivate",
                       "amazingDirectMethod");

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (.dbg DBG_SET_PROLOGUE_END)
      (.pos:dbg_0 "LABExperimentContextTest;.getNum:()I" ABExperimentContextTest.java 14)
      (invoke-direct (v1) "LABExperimentContextTest;.amazingDirectMethod:()I")
      (move-result v0)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(ABExperimentContextTest, testControlMode) {
  ab_test::ABExperimentContextImpl::parse_experiments_states(
      {{"ab_experiment", "control"}});
  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);
  m->get_code()->build_cfg(true);
  change_called_method("ab_experiment", m, "getSixPrivate",
                       "amazingDirectMethod");

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (.dbg DBG_SET_PROLOGUE_END)
      (.pos:dbg_0 "LABExperimentContextTest;.getNum:()I" ABExperimentContextTest.java 14)
      (invoke-direct (v1) "LABExperimentContextTest;.getSixPrivate:()I")
      (move-result v0)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

} // namespace ab_test
