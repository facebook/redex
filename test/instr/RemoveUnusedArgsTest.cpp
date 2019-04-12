/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Resolver.h"
#include "VerifyUtil.h"

namespace {

// Checks only the first callsite in method against num_args_expected
void check_callsite_regs(DexMethod* method, int num_args_expected) {
  for (const auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto actual_method = insn->get_method();
      EXPECT_EQ(insn->arg_word_count(), num_args_expected)
          << show(actual_method);
      break;
    }
  }
}

// Sanity check: three foo constructors are defined
TEST_F(PreVerify, CtorsDefined) {
  auto foo = find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo;");
  ASSERT_NE(nullptr, foo);

  auto ctors = foo->get_ctors();
  EXPECT_EQ(ctors.size(), 3);
  for (uint i = 0; i < 3; ++i) {
    auto ctor = ctors.at(i);
    ASSERT_NE(nullptr, ctor);
  }
}

// Check unused arguments are successfully removed in constructors
TEST_F(PreVerify, RemoveCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo = find_vmethod_named(*foo_user, "use_foo1");
  ASSERT_NE(nullptr, use_foo);
  use_foo->balloon();

  check_callsite_regs(use_foo, 2);
}

TEST_F(PostVerify, RemoveCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo = find_vmethod_named(*foo_user, "use_foo1");
  ASSERT_NE(nullptr, use_foo);
  use_foo->balloon();

  check_callsite_regs(use_foo, 1);
}

// Check arguments of constructor aren't removed when they're used
TEST_F(PreVerify, DontRemoveUsedCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo2 = find_vmethod_named(*foo_user, "use_foo2");
  ASSERT_NE(nullptr, use_foo2);
  use_foo2->balloon();

  check_callsite_regs(use_foo2, 3);
}

TEST_F(PostVerify, DontRemoveUsedCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo2 = find_vmethod_named(*foo_user, "use_foo2");
  ASSERT_NE(nullptr, use_foo2);
  use_foo2->balloon();

  check_callsite_regs(use_foo2, 3);
}

// Check arguments of constructor aren't removed when the signature collides
TEST_F(PreVerify, CollidingCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo3 = find_vmethod_named(*foo_user, "use_foo3");
  ASSERT_NE(nullptr, use_foo3);
  use_foo3->balloon();

  check_callsite_regs(use_foo3, 4);
}

TEST_F(PostVerify, CollidingCtorArg) {
  auto foo_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FooUser;");
  ASSERT_NE(nullptr, foo_user);

  auto use_foo3 = find_vmethod_named(*foo_user, "use_foo3");
  ASSERT_NE(nullptr, use_foo3);
  use_foo3->balloon();

  check_callsite_regs(use_foo3, 4);
}

// Check no-argument static methods' invokes don't change
TEST_F(PreVerify, StaticsNoArgs) {
  auto statics =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Statics;");
  ASSERT_NE(nullptr, statics);

  auto static1 = find_dmethod_named(*statics, "static1");
  ASSERT_NE(nullptr, static1);

  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static1 = find_vmethod_named(*statics_user, "use_static1");
  ASSERT_NE(nullptr, use_static1);
  use_static1->balloon();

  check_callsite_regs(use_static1, 0);
}

TEST_F(PostVerify, StaticsNoArgs) {
  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static1 = find_vmethod_named(*statics_user, "use_static1");
  ASSERT_NE(nullptr, use_static1);
  use_static1->balloon();

  check_callsite_regs(use_static1, 0);
}

// Check static methods' invokes don't change when args are used
TEST_F(PreVerify, StaticsUsedArgs) {
  auto statics =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Statics;");
  ASSERT_NE(nullptr, statics);

  auto static2 = find_dmethod_named(*statics, "static2");
  ASSERT_NE(nullptr, static2);

  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static2 = find_vmethod_named(*statics_user, "use_static2");
  ASSERT_NE(nullptr, use_static2);
  use_static2->balloon();

  check_callsite_regs(use_static2, 1);
}

TEST_F(PostVerify, StaticsUsedArgs) {
  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static2 = find_vmethod_named(*statics_user, "use_static2");
  ASSERT_NE(nullptr, use_static2);
  use_static2->balloon();

  check_callsite_regs(use_static2, 1);
}

// Check static method arg removal for unused args
TEST_F(PreVerify, StaticsUnusedArgs) {
  auto statics =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Statics;");
  ASSERT_NE(nullptr, statics);

  auto static3 = find_dmethod_named(*statics, "static3");
  ASSERT_NE(nullptr, static3);

  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static3 = find_vmethod_named(*statics_user, "use_static3");
  ASSERT_NE(nullptr, use_static3);
  use_static3->balloon();

  check_callsite_regs(use_static3, 2);
}

TEST_F(PostVerify, StaticsUnusedArgs) {
  auto statics_user =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/StaticsUser;");
  ASSERT_NE(nullptr, statics_user);

  auto use_static3 = find_vmethod_named(*statics_user, "use_static3");
  ASSERT_NE(nullptr, use_static3);
  use_static3->balloon();

  check_callsite_regs(use_static3, 1);
}

// Check overloaded name mangling upon collision
TEST_F(PreVerify, PrivatesUsedArgs) {
  auto privates =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Privates;");
  ASSERT_NE(nullptr, privates);
  Scope scope = {privates};
  balloon_for_test(scope);

  // 1 default constructor + 2 private void private1() methods
  auto dmethods = privates->get_dmethods();
  EXPECT_EQ(dmethods.size(), 3);

  bool two_args_method = false;
  bool three_args_method = false;

  for (auto dmethod : dmethods) {
    if (!is_constructor(dmethod)) {
      auto num_args = dmethod->get_proto()->get_args()->get_type_list().size();
      if (num_args == 2) {
        two_args_method = true;
      } else {
        three_args_method = true;
      }
    }
  }

  EXPECT_TRUE(two_args_method && three_args_method);
  auto use_private_first = find_vmethod_named(*privates, "use_private_first");
  check_callsite_regs(use_private_first, 3);
  auto use_private_second = find_vmethod_named(*privates, "use_private_second");
  check_callsite_regs(use_private_second, 4);
}

TEST_F(PostVerify, PrivatesUsedArgs) {
  auto privates =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Privates;");
  ASSERT_NE(nullptr, privates);
  Scope scope = {privates};
  balloon_for_test(scope);

  auto dmethods = privates->get_dmethods();
  EXPECT_EQ(dmethods.size(), 3);

  std::vector<DexMethod*> overloaded_methods;
  for (auto dmethod : dmethods) {
    if (!is_constructor(dmethod)) {
      auto num_args = dmethod->get_proto()->get_args()->get_type_list().size();
      EXPECT_EQ(num_args, 2);
      overloaded_methods.emplace_back(dmethod);
    }
  }

  EXPECT_EQ(overloaded_methods.size(), 2);
  auto name1 = overloaded_methods.at(0)->get_name()->str();
  auto name2 = overloaded_methods.at(1)->get_name()->str();
  ASSERT_NE(name1, name2);

  auto use_private_first = find_vmethod_named(*privates, "use_private_first");
  check_callsite_regs(use_private_first, 3);
  auto use_private_second = find_vmethod_named(*privates, "use_private_second");
  check_callsite_regs(use_private_second, 3);
}

// Check nonvirtual method arg removal for unused args
TEST_F(PreVerify, PublicNonVirtualsUnusedArgs) {
  auto non_virtuals =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NonVirtuals;");
  ASSERT_NE(nullptr, non_virtuals);

  auto non_virtual1 = find_vmethod_named(*non_virtuals, "non_virtual1");
  ASSERT_NE(nullptr, non_virtual1);
  auto code = non_virtual1->get_code();

  auto non_virtuals_user = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/NonVirtualsUser;");
  ASSERT_NE(nullptr, non_virtuals_user);

  auto use_non_virtual1 =
      find_vmethod_named(*non_virtuals_user, "use_non_virtual1");
  ASSERT_NE(nullptr, use_non_virtual1);
  use_non_virtual1->balloon();

  check_callsite_regs(use_non_virtual1, 2);
}

TEST_F(PostVerify, NonVirtualsUnusedArgs) {
  auto non_virtuals =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NonVirtuals;");
  ASSERT_NE(nullptr, non_virtuals);

  auto non_virtual1 = find_vmethod_named(*non_virtuals, "non_virtual1$uva0");
  ASSERT_NE(nullptr, non_virtual1);

  auto non_virtuals_user = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/NonVirtualsUser;");
  ASSERT_NE(nullptr, non_virtuals_user);

  auto use_non_virtual1 =
      find_vmethod_named(*non_virtuals_user, "use_non_virtual1");
  ASSERT_NE(nullptr, use_non_virtual1);
  use_non_virtual1->balloon();

  check_callsite_regs(use_non_virtual1, 1);
}

// Check protected method arg removal for unused args
TEST_F(PreVerify, ProtectedNonVirtualsUnusedArgs) {
  auto non_virtuals =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NonVirtuals;");
  ASSERT_NE(nullptr, non_virtuals);

  auto non_virtual2 = find_vmethod_named(*non_virtuals, "non_virtual2");
  ASSERT_NE(nullptr, non_virtual2);

  auto non_virtuals_user = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/NonVirtualsUser;");
  ASSERT_NE(nullptr, non_virtuals_user);

  auto use_non_virtual2 =
      find_vmethod_named(*non_virtuals_user, "use_non_virtual2");
  ASSERT_NE(nullptr, use_non_virtual2);
  use_non_virtual2->balloon();

  check_callsite_regs(use_non_virtual2, 2);
}

TEST_F(PostVerify, ProtectedNonVirtualsUnusedArgs) {
  auto non_virtuals =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NonVirtuals;");
  ASSERT_NE(nullptr, non_virtuals);

  auto non_virtual2 = find_vmethod_named(*non_virtuals, "non_virtual2$uva0");
  ASSERT_NE(nullptr, non_virtual2);

  auto non_virtuals_user = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/NonVirtualsUser;");
  ASSERT_NE(nullptr, non_virtuals_user);

  auto use_non_virtual2 =
      find_vmethod_named(*non_virtuals_user, "use_non_virtual2");
  ASSERT_NE(nullptr, use_non_virtual2);
  use_non_virtual2->balloon();

  check_callsite_regs(use_non_virtual2, 1);
}

} // namespace
