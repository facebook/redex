/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexUtil.h"
#include "Match.h"
#include "Resolver.h"
#include "VerifyUtil.h"

namespace {

bool has_method_invoke(DexMethod* method, DexMethod* callee) {
  auto code = method->get_code();

  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (!is_invoke(insn->opcode())) {
      continue;
    }

    auto mdef = resolve_method(insn->get_method(), opcode_to_search(insn));
    if (!mdef) {
      continue;
    }

    if (mdef == callee) {
      return true;
    }
  }

  return false;
}

} // namespace

TEST_F(PreVerify, testInvokeVirtualReplaced) {
  auto root = find_class_named(classes, "Lcom/facebook/redextest/rebind/Root;");
  EXPECT_NE(nullptr, root);

  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  EXPECT_NE(nullptr, body);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeVirtualReplaced");
  EXPECT_NE(nullptr, method);

  auto foo_root_method = find_vmethod_named(*root, "foo");
  EXPECT_NE(nullptr, foo_root_method);

  auto foo_body_method = find_vmethod_named(*body, "foo");
  EXPECT_NE(nullptr, foo_body_method);

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, foo_root_method));
  EXPECT_FALSE(has_method_invoke(method, foo_body_method));
}

TEST_F(PostVerify, testInvokeVirtualReplaced) {
  auto root = find_class_named(classes, "Lcom/facebook/redextest/rebind/Root;");
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeVirtualReplaced");
  auto foo_body_method = find_vmethod_named(*body, "foo");
  auto foo_root_method = find_vmethod_named(*root, "foo");

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, foo_body_method));
  EXPECT_FALSE(has_method_invoke(method, foo_root_method));
}

TEST_F(PreVerify, testInvokeVirtualSkipped) {
  auto root = find_class_named(classes, "Lcom/facebook/redextest/rebind/Root;");
  EXPECT_NE(nullptr, root);

  auto leaf = find_class_named(classes, "Lcom/facebook/redextest/rebind/Leaf;");
  EXPECT_NE(nullptr, leaf);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeVirtualSkipped");
  EXPECT_NE(nullptr, method);

  auto foo_root_method = find_vmethod_named(*root, "foo");
  EXPECT_NE(nullptr, foo_root_method);

  auto foo_leaf_method = find_vmethod_named(*leaf, "foo");
  EXPECT_EQ(nullptr, foo_leaf_method);

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, foo_root_method));
}

TEST_F(PostVerify, testInvokeVirtualSkipped) {
  auto root = find_class_named(classes, "Lcom/facebook/redextest/rebind/Root;");
  auto leaf = find_class_named(classes, "Lcom/facebook/redextest/rebind/leaf;");
  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeVirtualSkipped");
  auto foo_root_method = find_vmethod_named(*root, "foo");

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, foo_root_method));
}

TEST_F(PreVerify, testInvokeInterfaceReplaced) {
  auto root_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/RootInterface;");
  EXPECT_NE(nullptr, root_intf);

  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  EXPECT_NE(nullptr, body);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeInterfaceReplaced");
  EXPECT_NE(nullptr, method);

  auto bar_root_intf_method = find_vmethod_named(*root_intf, "bar");
  EXPECT_NE(nullptr, bar_root_intf_method);

  auto bar_body_method = find_vmethod_named(*body, "bar");
  EXPECT_NE(nullptr, bar_body_method);

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, bar_root_intf_method));
  EXPECT_FALSE(has_method_invoke(method, bar_body_method));

  auto leaf_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/LeafInterface;");
  EXPECT_NE(nullptr, leaf_intf);

  auto leaf = find_class_named(classes, "Lcom/facebook/redextest/rebind/Leaf;");
  EXPECT_NE(nullptr, leaf);

  auto car_leaf_intf_method = find_vmethod_named(*leaf_intf, "car");
  EXPECT_NE(nullptr, car_leaf_intf_method);

  auto car_leaf_method = find_vmethod_named(*leaf, "car");
  EXPECT_NE(nullptr, car_leaf_method);

  EXPECT_TRUE(has_method_invoke(method, car_leaf_intf_method));
  EXPECT_FALSE(has_method_invoke(method, car_leaf_method));
}

TEST_F(PostVerify, testInvokeInterfaceReplaced) {
  auto root_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/RootInterface;");
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeInterfaceReplaced");
  auto bar_body_method = find_vmethod_named(*body, "bar");
  auto bar_root_intf_method = find_vmethod_named(*root_intf, "bar");

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, bar_body_method));
  EXPECT_FALSE(has_method_invoke(method, bar_root_intf_method));

  auto leaf_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/LeafInterface;");
  auto leaf = find_class_named(classes, "Lcom/facebook/redextest/rebind/Leaf;");
  auto car_leaf_intf_method = find_vmethod_named(*leaf_intf, "car");
  auto car_leaf_method = find_vmethod_named(*leaf, "car");
  EXPECT_TRUE(has_method_invoke(method, car_leaf_method));
  EXPECT_FALSE(has_method_invoke(method, car_leaf_intf_method));
}

TEST_F(PreVerify, testInvokeInterfaceSkipped) {
  auto body_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/BodyInterface;");
  EXPECT_NE(nullptr, body_intf);

  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  EXPECT_NE(nullptr, body);

  auto dar_body_intf_method = find_vmethod_named(*body_intf, "dar");
  EXPECT_NE(nullptr, dar_body_intf_method);

  auto dar_body_method = find_vmethod_named(*body, "dar");
  EXPECT_NE(nullptr, dar_body_method);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeInterfaceSkipped");
  EXPECT_NE(nullptr, method);

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, dar_body_intf_method));
}

TEST_F(PostVerify, testInvokeInterfaceSkipped) {
  auto body_intf = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/BodyInterface;");
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto dar_body_intf_method = find_vmethod_named(*body_intf, "dar");

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/rebind/ReBindVRefsTest;");
  auto method = find_vmethod_named(*test_cls, "testInvokeInterfaceSkipped");
  EXPECT_NE(nullptr, method);

  method->balloon();
  EXPECT_TRUE(has_method_invoke(method, dar_body_intf_method));
}

TEST_F(PreVerify, testInvokeSuperReplaced) {
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto invoke_super_final = find_vmethod_named(*body, "invoke_super_final");
  auto invoke = static_cast<DexOpcodeMethod*>(
      find_instruction(invoke_super_final, DOPCODE_INVOKE_SUPER));
  ASSERT_NE(invoke, nullptr);
  EXPECT_EQ(invoke->get_method(),
            DexMethod::get_method(
                "Lcom/facebook/redextest/rebind/Root;.final_method:()I"));
}

TEST_F(PostVerify, testInvokeSuperReplaced) {
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto invoke_super_final = find_vmethod_named(*body, "invoke_super_final");
  auto invoke = static_cast<DexOpcodeMethod*>(
      find_instruction(invoke_super_final, DOPCODE_INVOKE_VIRTUAL));
  ASSERT_NE(invoke, nullptr);
  EXPECT_EQ(invoke->get_method(),
            DexMethod::get_method(
                "Lcom/facebook/redextest/rebind/Root;.final_method:()I"));
}

TEST_F(PostVerify, testInvokeSuperNotReplaced) {
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto invoke_super_nonfinal =
      find_vmethod_named(*body, "invoke_super_nonfinal");
  auto invoke = static_cast<DexOpcodeMethod*>(
      find_instruction(invoke_super_nonfinal, DOPCODE_INVOKE_SUPER));
  ASSERT_NE(invoke, nullptr);
  EXPECT_EQ(
      invoke->get_method(),
      DexMethod::get_method("Lcom/facebook/redextest/rebind/Root;.bar:()I"));
}

TEST_F(PostVerify, testInvokeSuperExternalFinalNotReplaced) {
  auto body = find_class_named(classes, "Lcom/facebook/redextest/rebind/Body;");
  auto invoke_super_nonfinal =
      find_vmethod_named(*body, "invoke_super_external_final");
  auto invoke = static_cast<DexOpcodeMethod*>(
      find_instruction(invoke_super_nonfinal, DOPCODE_INVOKE_SUPER));
  ASSERT_NE(invoke, nullptr);
  EXPECT_EQ(
      invoke->get_method(),
      DexMethod::get_method("Ljava/lang/Object;.getClass:()Ljava/lang/Class;"));
}
