/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "PureMethods.h"
#include "RedexTest.h"
#include "VirtualScope.h"
#include "Walkers.h"

class PureAnalysisTest : public RedexTest {
 public:
  PureAnalysisTest() { get_vmethods(type::java_lang_Object()); }
};

void test(const char* signature, const std::string& code_str, bool is_pure) {

  auto field_a = DexField::make_field("LFoo;.a:I")->make_concrete(ACC_PUBLIC);
  auto field_b = DexField::make_field("LBar;.a:I")->make_concrete(ACC_PUBLIC);
  ClassCreator creator1(DexType::make_type("LFoo;"));
  ClassCreator creator2(DexType::make_type("LBar;"));

  creator1.set_super(type::java_lang_Object());
  creator2.set_super(type::java_lang_Object());
  auto method1 = static_cast<DexMethod*>(DexMethod::make_method(signature));
  method1->set_access(ACC_PUBLIC);
  method1->set_external();
  method1->set_code(assembler::ircode_from_string(code_str));
  method1->get_code()->build_cfg();
  creator1.add_method(method1);
  creator1.add_field(field_a);
  creator2.add_field(field_b);

  auto method2 =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.add:()V"));
  method2->set_access(ACC_PUBLIC);
  method2->set_virtual(true);
  method2->set_external();
  creator1.add_method(method2);
  Scope scope{type_class(type::java_lang_Object()), creator1.create(),
              creator2.create()};

  AnalyzePureMethodsPass pass;
  pass.analyze_and_set_pure_methods(scope);
  auto code = assembler::ircode_from_string(code_str);

  if (is_pure) {
    EXPECT_EQ(method1->rstate.pure_method(), true)
        << assembler::to_string(code.get()).c_str();
  } else {
    EXPECT_EQ(method1->rstate.pure_method(), false)
        << assembler::to_string(code.get()).c_str();
  }
}

// Pure function
TEST_F(PureAnalysisTest, simple1) {
  auto code_str = R"(
    (
      (add-int v1 v1 v2)
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(II)I";
  test(signature, code_str, true);
}

// Not pure function; accesses field
TEST_F(PureAnalysisTest, simple2) {
  auto code_str = R"(
    (
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (add-int v1 v1 v2)
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(II)I";
  test(signature, code_str, false);
}

// Not pure function; May escape through method call
TEST_F(PureAnalysisTest, simple3) {
  auto code_str = R"(
    (
      (add-int v1 v1 v2)
      (invoke-virtual (v0) "LFoo;.add:()V")
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(II)I";
  test(signature, code_str, false);
}

// Pure function; returns the object as it is
TEST_F(PureAnalysisTest, simple4) {
  auto code_str = R"(
    (
      (return-object v1)
    )
  )";
  const char* signature = "LFoo;.add:(LBar;)LBar;";
  test(signature, code_str, true);
}

// Pure function; returns this
TEST_F(PureAnalysisTest, simple5) {
  auto code_str = R"(
    (
      (return-object v0)
    )
  )";
  const char* signature = "LFoo;.add:()LFoo;";
  test(signature, code_str, true);
}

// Pure function; Reads param object
TEST_F(PureAnalysisTest, simple6) {
  auto code_str = R"(
    (
      (load-param-object v1)
      (iget v1 "LBar;.a:I")
      (move-result-pseudo v2)
      (add-int v1 v2 v2)
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(LBar;)I;";
  test(signature, code_str, true);
}

// Not pure function; Reads param object and field
TEST_F(PureAnalysisTest, simple7) {
  auto code_str = R"(
    (
      (load-param-object v1)
      (iget v1 "LBar;.a:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (add-int v1 v1 v2)
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(LBar;)I;";
  test(signature, code_str, false);
}

// Not pure function; modifies param object and returns
TEST_F(PureAnalysisTest, simple8) {
  auto code_str = R"(
    (
      (load-param-object v1)
      (const v2 0)
      (iput v2 v1 "LBar;.a:I")
      (return v1)
    )
  )";
  const char* signature = "LFoo;.add:(LBar;)Lbar;";
  test(signature, code_str, false);
}

// Not pure function; modifies param object
TEST_F(PureAnalysisTest, simple9) {
  auto code_str = R"(
    (
      (load-param-object v1)
      (const v2 0)
      (iput v2 v1 "LBar;.a:I")
      (return-void)
    )
  )";
  const char* signature = "LFoo;.add:(LBar;)V;";
  test(signature, code_str, false);
}
