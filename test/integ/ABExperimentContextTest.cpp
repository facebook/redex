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

#include <json/json.h>

namespace ab_test {

struct ABExperimentContextTest : public RedexIntegrationTest {
  void SetUp() override { ABExperimentContextImpl::reset_global_state(); }
};

void set_state_for_experiments(
    const std::vector<std::string>& exp_names,
    const std::string& state,
    const boost::optional<std::string>& default_state = boost::none) {
  std::string config = "{\"ab_experiments_states\":{";

  for (uint32_t i = 0; i < exp_names.size(); i++) {
    config += "\"" + exp_names[i] + "\":\"" + state + "\"";
    if (i != exp_names.size() - 1) {
      config += ",";
    }
  }

  config += "},";

  config += "\"ab_experiments_default\":\"";
  config += default_state ? *default_state : "test";

  config += "\"";

  config += "}\n";

  Json::Value json_conf;
  std::istringstream temp_json(config);

  temp_json >> json_conf;

  ConfigFiles conf_files(json_conf);
  ab_test::ABExperimentContextImpl::parse_experiments_states(conf_files);
}

void change_called_method(const std::string& exp_name,
                          DexMethod* m,
                          const std::string& original_method_name,
                          const std::string& new_method_name) {
  ab_test::ABExperimentContextImpl experiment(exp_name);
  if (experiment.use_control()) {
    return;
  }

  experiment.try_register_method(m);

  m->get_code()->build_cfg();
  auto& cfg = m->get_code()->cfg();
  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    IRInstruction* insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {

      auto m_ref = insn->get_method();
      if (m_ref->get_name()->str() == original_method_name) {
        auto name = DexString::make_string(new_method_name);
        DexMethodRef* method = DexMethod::make_method(m_ref->get_class(), name,
                                                      m_ref->get_proto());
        insn->set_method(method);
        break;
      }
    }
  }
  m->get_code()->clear_cfg();
  experiment.flush();
}

TEST_F(ABExperimentContextTest, testCFGConstructorBasicFunctionality) {
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("basicMethod");
  ASSERT_TRUE(m != nullptr);

  ab_test::ABExperimentContextImpl experiment("ab_experiment");
  experiment.try_register_method(m);
  m->get_code()->build_cfg();
  experiment.flush();
}

TEST_F(ABExperimentContextTest, testTestingMode) {
  set_state_for_experiments({"ab_experiment"}, "test");

  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);

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

TEST_F(ABExperimentContextTest, testTestingModeDefault) {
  set_state_for_experiments({}, "", boost::optional<std::string>("test"));

  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);

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
  set_state_for_experiments({"ab_experiment"}, "control");
  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);

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

TEST_F(ABExperimentContextTest, testControlModeDefault) {
  set_state_for_experiments({}, "", boost::optional<std::string>("control"));
  ASSERT_TRUE(classes);
  DexMethod* m =
      (*classes)[0]->find_method_from_simple_deobfuscated_name("getNum");
  ASSERT_TRUE(m != nullptr);

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
