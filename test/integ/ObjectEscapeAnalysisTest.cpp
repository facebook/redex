/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
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
};

TEST_F(ObjectEscapeAnalysisTest, reduceTo42A) {
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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

TEST_F(ObjectEscapeAnalysisTest, doNotReduceTo42A) {
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
  std::vector<Pass*> passes = {
      new ObjectEscapeAnalysisPass(),
  };

  run_passes(passes);

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
