/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <unistd.h>

#include "BranchPrefixHoistingPass.h"
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
#include "Show.h"
#include "VirtualScope.h"

class ObjectEscapeAnalysisTest : public RedexIntegrationTest {
 public:
  ObjectEscapeAnalysisTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    virt_scope::get_vmethods(type::java_lang_Object());

    auto* object_ctor =
        static_cast<DexMethod*>(method::java_lang_Object_ctor());
    object_ctor->set_access(ACC_PUBLIC | ACC_CONSTRUCTOR);
    object_ctor->set_external();
    type_class(type::java_lang_Object())->add_method(object_ctor);
    type_class(type::java_lang_Object())->set_external();
  }

  void run() {
    auto* config_file_env = std::getenv("config_file");
    always_assert_log(
        config_file_env,
        "Config file must be specified to ObjectEscapeAnalysisTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;

    std::vector<Pass*> passes = {
        new BranchPrefixHoistingPass(),
        new ObjectEscapeAnalysisPass(/* register_plugins */ false),
    };

    run_passes(passes, nullptr, cfg);
  }

  sparta::s_expr get_s_expr(const char* method_name) {
    auto* method = DexMethod::get_method(method_name);
    always_assert(method);
    always_assert(method->is_def());
    auto* code = method->as_def()->get_code();
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
    auto* method = DexMethod::get_method(method_name);
    always_assert(method);
    always_assert(method->is_def());
    auto* code = method->as_def()->get_code();
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

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithOverrides) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithOverrides:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithOverrides2) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithOverrides2:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v2 42)
      (return v2)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithInvokeSuper) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithInvokeSuper:()I");
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

TEST_F(ObjectEscapeAnalysisTest, optionalReduceTo42) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceTo42:(Z)I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v2)
      (if-eqz v2 :L1)
      (const v1 42)
      (:L0)
      (return v1)
      (:L1)
      (const v1 0)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, optionalReduceTo42Alt) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceTo42Alt:(Z)I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v2)
      (if-eqz v2 :L1)
      (const v1 42)
      (:L0)
      (return v1)
      (:L1)
      (const v1 0)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, optionalReduceTo42Override) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceTo42Override:(Z)I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v2)
      (if-eqz v2 :L1)
      (const v1 0)
      (:L0)
      (return v1)
      (:L1)
      (const v1 42)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, optionalReduceTo42CheckCast) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceTo42CheckCast:(Z)I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v2)
      (if-eqz v2 :L1)
      (const v1 0)
      (:L0)
      (return v1)
      (:L1)
      (const v1 42)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

// This test case documents how we suppress NPEs. This is a tolerated artefact
// of the transformation, not something we have to do.
TEST_F(ObjectEscapeAnalysisTest, optionalReduceTo42SuppressNPE) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceTo42SuppressNPE:(Z)I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v2)
      (const v11 0)
      (if-eqz v2 :L0)
      (const v11 42)
      (:L0)
      (return v11)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, optionalReduceToBC) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalReduceToBC:(ZZ)Z");
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v3)
      (load-param v4)
      (if-eqz v3 :L2)
      (const v9 1)
      (:L0)
      (const v10 0)
      (if-eqz v4 :L1)
      (move v10 v9)
      (:L1)
      (return v10)
      (:L2)
      (const v9 0)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, optionalLoopyReduceTo42) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.optionalLoopyReduceTo42:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v11 0)
      (const v0 0)
      (:L0)
      (if-eqz v0 :L2)
      (const v2 2)
      (if-ne v0 v2 :L1)
      (return v11)
      (:L1)
      (const v11 42)
      (:L2)
      (add-int/lit v0 v0 1)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, objectIsNotNull) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.objectIsNotNull:()Z");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 0) (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithCheckCast) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithCheckCast:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithReturnedArg) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithReturnedArg:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (const v1 42)
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
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;")
      (move-result-pseudo-object v0)
      (const v3 42)
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

TEST_F(ObjectEscapeAnalysisTest, reduceTo42WithExpandedMethod) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceTo42WithExpandedMethod:()V");
  auto expected = assembler::ircode_from_string(R"(
    (
      (const v2 42)
      (invoke-static (v2) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$N;.onlyUseInstanceField$oea$0:(I)V")
      (return-void)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());

  auto actual_expanded_method = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest$N;.onlyUseInstanceField$oea$0:(I)V");
  auto expected_expanded_ctor = assembler::ircode_from_string(R"(
    (
      (load-param v3)
      (const v2 0)
      (sget-object "Ljava/lang/System;.out:Ljava/io/PrintStream;")
      (move-result-pseudo-object v0)
      (move v1 v3)
      (invoke-virtual (v0 v1) "Ljava/io/PrintStream;.println:(I)V")
      (return-void)
    )
  )");
  ASSERT_EQ(actual_expanded_method.str(),
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
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;")
      (move-result-pseudo-object v1)
      (const v2 16)
      (invoke-direct (v1 v2) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;.<init>:(I)V")
      (sput-object v1 "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;.instance:Lcom/facebook/redextest/ObjectEscapeAnalysisTest$O;")
      (const v1 42)
      (return v1)
   )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, reduceIncompleteInlinableType) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.reduceIncompleteInlinableType:(Z)I");
  // We reduce away the creation of the "O" type, but not yet the D type, as
  // that's a second-order problem...
  auto expected = assembler::ircode_from_string(R"(
   (
      (load-param v5)
      (const v8 42)
      (invoke-static (v8) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$D;.allocator:(I)Lcom/facebook/redextest/ObjectEscapeAnalysisTest$D;")
      (move-result-object v6)
      (invoke-virtual (v6) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$D;.getX:()I")
      (if-eqz v5 :L1)
      (const v23 42)
      (:L0)
      (return v23)
      (:L1)
      (const v23 23)
      (goto :L0)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, doNotReduceTo42Finalize) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.doNotReduceTo42Finalize:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$P;")
      (move-result-pseudo-object v0)
      (const v1 42)
      (invoke-direct (v0 v1) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$P;.<init>:(I)V")
      (invoke-virtual (v0) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$P;.getX:()I")
      (move-result v1)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, doNotReduceTo42FinalizeDervied) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.doNotReduceTo42FinalizeDerived:()I");
  auto expected = assembler::ircode_from_string(R"(
   (
      (new-instance "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$PDerived;")
      (move-result-pseudo-object v0)
      (const v1 42)
      (invoke-direct (v0 v1) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$PDerived;.<init>:(I)V")
      (invoke-virtual (v0) "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$PDerived;.getX:()I")
      (move-result v1)
      (return v1)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}

TEST_F(ObjectEscapeAnalysisTest, nothingToReduce) {
  run();

  auto actual = get_s_expr(
      "Lcom/facebook/redextest/"
      "ObjectEscapeAnalysisTest;.nothingToReduce:()V");
  auto expected = assembler::ircode_from_string(R"(
   (
      (invoke-static () "Lcom/facebook/redextest/ObjectEscapeAnalysisTest$Q;.allocator:()Lcom/facebook/redextest/ObjectEscapeAnalysisTest$Q;")
      (return-void)
    )
)");
  ASSERT_EQ(actual.str(), assembler::to_s_expr(expected.get()).str());
}
