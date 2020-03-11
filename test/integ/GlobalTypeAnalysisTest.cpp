/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "GlobalTypeAnalyzer.h"
#include "RedexTest.h"

using namespace type_analyzer;
using namespace type_analyzer::global;

class GlobalTypeAnalysisTest : public RedexIntegrationTest {
 protected:
  void set_root_method(const std::string& full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    method->rstate.set_root();
  }

  DexMethod* get_method(const std::string& name) {
    std::string full_name = "Lcom/facebook/redextest/" + name;
    return DexMethod::get_method(full_name)->as_def();
  }

  DexMethod* get_method(const std::string& name, const std::string& rtype) {
    std::string full_name = "Lcom/facebook/redextest/" + name +
                            ":()Lcom/facebook/redextest/" + rtype + ";";
    return DexMethod::get_method(full_name)->as_def();
  }

  DexMethod* get_method(const std::string& name,
                        const std::string& params,
                        const std::string& rtype) {
    std::string full_name = "Lcom/facebook/redextest/" + name + ":(" + params +
                            ")Lcom/facebook/redextest/" + rtype + ";";
    return DexMethod::get_method(full_name)->as_def();
  }

  DexTypeDomain get_type_domain(const std::string& type_name) {
    std::string full_name = "Lcom/facebook/redextest/" + type_name + ";";
    return DexTypeDomain(DexType::make_type(DexString::make_string(full_name)));
  }
};

TEST_F(GlobalTypeAnalysisTest, ReturnTypeTest) {
  auto scope = build_class_scope(stores);
  set_root_method("Lcom/facebook/redextest/TestA;.foo:()I");

  GlobalTypeAnalysis analysis;
  auto gta = analysis.analyze(scope);
  auto wps = gta->get_whole_program_state();

  auto meth_get_subone = get_method("TestA;.getSubOne", "Base");
  EXPECT_EQ(wps.get_return_type(meth_get_subone), get_type_domain("SubOne"));
  auto meth_get_subtwo = get_method("TestA;.getSubTwo", "Base");
  EXPECT_EQ(wps.get_return_type(meth_get_subtwo), get_type_domain("SubTwo"));
  auto meth_passthrough =
      get_method("TestA;.passThrough", "Lcom/facebook/redextest/Base;", "Base");
  EXPECT_EQ(wps.get_return_type(meth_passthrough), get_type_domain("SubTwo"));

  auto meth_foo = get_method("TestA;.foo:()I");
  auto lta = gta->get_local_analysis(meth_foo);
  auto code = meth_foo->get_code();
  auto foo_exit_env = lta->get_exit_state_at(code->cfg().exit_block());
  EXPECT_EQ(foo_exit_env.get_reg_environment().get(0),
            get_type_domain("SubOne"));
  EXPECT_EQ(foo_exit_env.get_reg_environment().get(2),
            get_type_domain("SubTwo"));
}
