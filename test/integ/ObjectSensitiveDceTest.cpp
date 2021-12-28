/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/thread/once.hpp>
#include <gtest/gtest.h>
#include <json/json.h>
#include <unordered_set>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

#include "ObjectSensitiveDcePass.h"
#include "RedexContext.h"
#include "Show.h"
#include "Trace.h"
#include "VirtualScope.h"
#include "Walkers.h"

class ObjectSensitiveDceTest : public RedexIntegrationTest {
 public:
  ObjectSensitiveDceTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(type::java_lang_Object());

    auto object_ctor = static_cast<DexMethod*>(method::java_lang_Object_ctor());
    object_ctor->set_access(ACC_PUBLIC | ACC_CONSTRUCTOR);
    object_ctor->set_external();
    type_class(type::java_lang_Object())->add_method(object_ctor);
  }
  void run_passes(const std::vector<Pass*>& passes) {
    RedexIntegrationTest::run_passes(passes);
  }
};

TEST_F(ObjectSensitiveDceTest, basic) {
  auto method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/ObjectSensitiveDceTest;.basic:()V");
  EXPECT_NE(method_ref, nullptr);
  auto method = method_ref->as_def();
  EXPECT_NE(method, nullptr);

  std::vector<Pass*> passes = {
      new ObjectSensitiveDcePass(),
  };

  run_passes(passes);

  auto ii = InstructionIterable(method->get_code());
  auto it = ii.begin();
  ASSERT_TRUE(it != ii.end());
  ASSERT_EQ(it->insn->opcode(), OPCODE_RETURN_VOID);
}

TEST_F(ObjectSensitiveDceTest, clinit_with_side_effects) {
  auto method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "ObjectSensitiveDceTest;.clinit_with_side_effects:()V");
  EXPECT_NE(method_ref, nullptr);
  auto method = method_ref->as_def();
  EXPECT_NE(method, nullptr);

  std::vector<Pass*> passes = {
      new ObjectSensitiveDcePass(),
  };

  run_passes(passes);

  auto ii = InstructionIterable(method->get_code());
  auto it = ii.begin();
  ASSERT_TRUE(it != ii.end());
  ASSERT_EQ(it->insn->opcode(), IOPCODE_INIT_CLASS);
  ASSERT_EQ(it->insn->get_type(),
            DexType::get_type(
                "Lcom/facebook/redextest/UselessWithClInitWithSideEffects;"));
  it++;
  ASSERT_EQ(it->insn->opcode(), OPCODE_RETURN_VOID);
}

TEST_F(ObjectSensitiveDceTest, method_needing_init_class) {
  auto method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "ObjectSensitiveDceTest;.method_needing_init_class:()V");
  EXPECT_NE(method_ref, nullptr);
  auto method = method_ref->as_def();
  EXPECT_NE(method, nullptr);

  std::vector<Pass*> passes = {
      new ObjectSensitiveDcePass(),
  };

  run_passes(passes);

  // Nothing could get optimized away, because the invoke-virtual to foo
  // triggers a clinit with side-effects, so it couldn't get removed, and thus
  // the object creation itself is required.
  auto code = method->get_code();
  ASSERT_EQ(method::count_opcode_of_types(code, {OPCODE_NEW_INSTANCE}), 1);
  ASSERT_EQ(method::count_opcode_of_types(code, {OPCODE_INVOKE_DIRECT}), 1);
  ASSERT_EQ(method::count_opcode_of_types(code, {OPCODE_INVOKE_VIRTUAL}), 1);
}
