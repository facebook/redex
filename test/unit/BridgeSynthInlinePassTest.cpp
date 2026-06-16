/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BridgeSynthInlineInternal.h"

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "RedexTest.h"
#include "TypeUtil.h"

class BridgeSynthInlinePassTest : public RedexTest {
 protected:
  // Returns a synthetic bridge whose body is `invoke-super Parent.foo()`;
  // parent + abstract method are registered as a side effect.
  DexMethod* make_synthetic_bridge_with_invoke_super_to_abstract(
      const std::string& parent_type_name, const std::string& child_type_name) {
    auto* foo_proto =
        DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));

    auto* parent_ty = DexType::make_type(parent_type_name);
    ClassCreator parent_cc(parent_ty);
    parent_cc.set_super(type::java_lang_Object());
    parent_cc.set_access(ACC_PUBLIC | ACC_ABSTRACT);
    auto* parent_foo = DexMethod::make_method(
                           parent_ty, DexString::make_string("foo"), foo_proto)
                           ->make_concrete(ACC_PUBLIC | ACC_ABSTRACT,
                                           /* is_virtual */ true);
    parent_cc.add_method(parent_foo);
    parent_cc.create();

    auto* child_ty = DexType::make_type(child_type_name);
    ClassCreator child_cc(child_ty);
    child_cc.set_super(parent_ty);
    auto* child_foo =
        DexMethod::make_method(
            child_ty, DexString::make_string("foo"), foo_proto)
            ->make_concrete(ACC_PUBLIC | ACC_BRIDGE | ACC_SYNTHETIC,
                            /* is_virtual */ true);
    child_foo->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (invoke-super (v0) ")" + parent_type_name + R"(.foo:()V")
        (return-void)
      )
    )"));
    child_cc.add_method(child_foo);
    child_cc.create();

    // Mirror the cfg-friendly pass contract that PassManager would set up.
    child_foo->get_code()->build_cfg();
    return child_foo;
  }

  static bool body_contains_opcode(IRCode* code, IROpcode op) {
    for (const auto& mie : cfg::InstructionIterable(code->cfg())) {
      if (mie.insn->opcode() == op) {
        return true;
      }
    }
    return false;
  }

  static bool body_throws_abstract_method_error(IRCode* code) {
    auto* ame = DexType::get_type("Ljava/lang/AbstractMethodError;");
    bool has_new_ame = false;
    bool has_throw = false;
    for (const auto& mie : cfg::InstructionIterable(code->cfg())) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_NEW_INSTANCE && insn->get_type() == ame) {
        has_new_ame = true;
      }
      if (insn->opcode() == OPCODE_THROW) {
        has_throw = true;
      }
    }
    return has_new_ame && has_throw;
  }
};

TEST_F(BridgeSynthInlinePassTest,
       rewrites_synthetic_bridge_invoke_super_to_abstract) {
  auto* bridge = make_synthetic_bridge_with_invoke_super_to_abstract(
      "LBaselineParent;", "LBaselineChild;");

  ASSERT_TRUE(is_bridge(bridge));
  ASSERT_TRUE(body_contains_opcode(bridge->get_code(), OPCODE_INVOKE_SUPER));

  EXPECT_TRUE(
      bridge_synth_inline_internal::rewrite_bridge_with_abstract_super_target(
          bridge));

  EXPECT_TRUE(body_throws_abstract_method_error(bridge->get_code()));
  EXPECT_FALSE(body_contains_opcode(bridge->get_code(), OPCODE_INVOKE_SUPER));
}

TEST_F(BridgeSynthInlinePassTest, honors_no_optimizations_flag) {
  auto* bridge = make_synthetic_bridge_with_invoke_super_to_abstract(
      "LNoOptParent;", "LNoOptChild;");

  bridge->rstate.set_no_optimizations();

  EXPECT_FALSE(
      bridge_synth_inline_internal::rewrite_bridge_with_abstract_super_target(
          bridge));

  EXPECT_TRUE(body_contains_opcode(bridge->get_code(), OPCODE_INVOKE_SUPER));
  EXPECT_FALSE(body_throws_abstract_method_error(bridge->get_code()));
}
