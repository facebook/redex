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
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "Show.h"
#include "UnreachableLoweringPass.h"
#include "VirtualScope.h"

class UnreachableLoweringPassTest : public RedexTest {
 public:
  ::sparta::s_expr run_pass(const std::string& code) {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    virt_scope::get_vmethods(type::java_lang_Object());

    std::string class_name = "LTest;";
    ClassCreator creator(DexType::make_type(class_name));
    creator.set_super(type::java_lang_Object());
    auto signature = class_name + ".foo:()V";
    auto* method = DexMethod::make_method(signature)->make_concrete(
        ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(assembler::ircode_from_string(code));
    creator.add_method(method);

    UnreachableLoweringPass pass;
    PassManager manager({&pass});
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
    auto actual_s_expr = run_pass(input);
    auto expected_s_expr = get_s_expr(expected);
    if (actual_s_expr == expected_s_expr) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << input << "\nevaluates to\n"
           << actual_s_expr.str() << "\ninstead of\n"
           << expected_s_expr.str();
  }
};

TEST_F(UnreachableLoweringPassTest, simple) {
  const auto* original_code = R"(
     (
      (unreachable v0)
      (throw v0)
     )
    )";
  const auto* expected_code = R"(
     (
      (invoke-static () "Lcom/redex/UnreachableException;.createAndThrow:()Lcom/redex/UnreachableException;")
      (move-result-object v0)
      (throw v0)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(UnreachableLoweringPassTest, move_objects_are_tolerated) {
  const auto* original_code = R"(
     (
      (unreachable v0)
      (move-object v1 v0)
      (throw v1)
     )
    )";
  const auto* expected_code = R"(
     (
      (invoke-static () "Lcom/redex/UnreachableException;.createAndThrow:()Lcom/redex/UnreachableException;")
      (move-result-object v0)
      (move-object v1 v0)
      (throw v1)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(UnreachableLoweringPassTest, invokes_are_tolerated) {
  const auto* original_code = R"(
     (
      (unreachable v0)
      (move-object v1 v0)
      (invoke-static () "Lcom/facebook/redex/dynamicanalysis/DynamicAnalysis;.onMethodExit:()V")
      (throw v1)
     )
    )";
  const auto* expected_code = R"(
     (
      (invoke-static () "Lcom/redex/UnreachableException;.createAndThrow:()Lcom/redex/UnreachableException;")
      (move-result-object v0)
      (move-object v1 v0)
      (invoke-static () "Lcom/facebook/redex/dynamicanalysis/DynamicAnalysis;.onMethodExit:()V")
      (throw v1)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}
