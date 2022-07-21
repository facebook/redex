/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <unistd.h>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRList.h"
#include "ObjectEscapeAnalysis.h"
#include "RedexTest.h"
#include "VirtualScope.h"

class ObjectEscapeAnalysisTest : public RedexIntegrationTest {
 public:
  ObjectEscapeAnalysisTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(type::java_lang_Object());

    auto object_ctor = static_cast<DexMethod*>(method::java_lang_Object_ctor());
    object_ctor->set_access(ACC_PUBLIC | ACC_CONSTRUCTOR);
    object_ctor->set_external();
    type_class(type::java_lang_Object())->add_method(object_ctor);
    type_class(type::java_lang_Object())->set_external();
  }

  void run() {
    auto config_file_env = std::getenv("config_file");
    always_assert_log(
        config_file_env,
        "Config file must be specified to ObjectEscapeAnalysisTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;

    std::vector<Pass*> passes = {
        new ObjectEscapeAnalysisPass(),
    };

    run_passes(passes, nullptr, cfg);
  }

  sparta::s_expr get_s_expr(const char* method_name) {
    auto method = DexMethod::get_method(method_name);
    always_assert(method);
    always_assert(method->is_def());
    auto code = method->as_def()->get_code();
    always_assert(code);
    for (auto ii = code->begin(); ii != code->end();) {
      if (ii->type == MFLOW_DEBUG || ii->type == MFLOW_POSITION) {
        ii = code->erase(ii);
      } else {
        ii++;
      }
    }
    return assembler::to_s_expr(code);
  }

  bool contains_invoke(const char* method_name) {
    auto method = DexMethod::get_method(method_name);
    always_assert(method);
    always_assert(method->is_def());
    auto code = method->as_def()->get_code();
    always_assert(code);
    for (auto& mie : InstructionIterable(code)) {
      if (opcode::is_an_invoke(mie.insn->opcode())) {
        return true;
      }
    }
    return false;
  }
};

TEST_F(ObjectEscapeAnalysisTest, reduceTo42A) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.reduceTo42A:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const-string "inlinable side effect")
      (move-result-pseudo-object v8)
      (sput-object v8 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.Foo:Ljava/lang/String;")

      (const-string "another inlinable side effect")
      (move-result-pseudo-object v4)
      (sput-object v4 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.Foo:Ljava/lang/String;")

      (const-string "yet another inlinable side effect")
      (move-result-pseudo-object v6)
      (sput-object v6 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.Foo:Ljava/lang/String;")

      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42B) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.reduceTo42B:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42C) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.reduceTo42C:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v2 42)
      (return v2)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42D) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.reduceTo42D:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, doNotReduceTo42A) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.doNotReduceTo42A:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$G;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$G;.<init>:()V")
      (invoke-virtual (v0) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$G;.getX:()I")
      (move-result v1)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, doNotReduceTo42B) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest;.doNotReduceTo42B:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$H;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$H;.<init>:()V")
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$G;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$G;.<init>:(Lcom/facebook/redextest/ObjectEscapeAnalysisTest$H;)V")
      (invoke-virtual (v1) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$H;.getX:()I")
      (move-result v2)
      (return v2)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42IdentityMatters) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42IdentityMatters:()Z");
  auto expected = assembler::ircode_from_string(R"(
   (
      (new-instance "Ljava/lang/Object;")
      (move-result-pseudo-object v2)
      (invoke-direct (v2) "Ljava/lang/Object;.<init>:()V")

      (const v1 0)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, DontOptimizeFinalInInit) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest$DontOptimizeFinalInInit;.<init>:()V");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param-object v3)
      (invoke-direct (v3) "Ljava/lang/Object;.<init>:()V")
      (const v2 42)
      (iput v2 v3 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$DontOptimizeFinalInInit;.x:I")
      (iput v2 v3 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$DontOptimizeFinalInInit;.y:I")
      (return-void)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithInitClass) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithInitClass:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (init-class "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$K;")

      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithMonitors) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithMonitors:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithMultiples) {
  run();

  auto actual1_contains_invoke = contains_invoke(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithMultiples1:(I)I");
  ASSERT_FALSE(actual1_contains_invoke);

  auto actual2_contains_invoke = contains_invoke(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithMultiples2:(I)I");
  ASSERT_FALSE(actual2_contains_invoke);
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithExpandedCtor) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithExpandedCtor:()Lcom/facebook/"
      "redextest/ObjectEscapeAnalysisTest$N;");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v3 42)
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v3) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;.<init>:(I)V")
      (return-object v0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());

  auto actual_expanded_ctor = get_s_expr(
      "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;.<init>:(I)V");
  auto expected_expanded_ctor = assembler::ircode_from_string(R"(
   (
      (load-param-object v1)
      (load-param v3)
      (const v2 0)
      (invoke-direct (v1) "Ljava/lang/Object;.<init>:()V")
      (move v0 v3)
      (iput v0 v1 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;.x:I")
      (return-void)
    )
)");
  ASSERT_EQ(actual_expanded_ctor.str(),
            assembler::to_s_expr(expected_expanded_ctor.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42IncompleteInlinableType) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42IncompleteInlinableType:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42IncompleteInlinableTypeB) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42IncompleteInlinableTypeB:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v2 16)
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1 v2) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;.<init>:(I)V")
      (sput-object v1 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;.instance:Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;")
      (const v1 42)
      (return v1)
   )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}
