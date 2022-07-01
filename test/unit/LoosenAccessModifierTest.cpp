/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "IRAssembler.h"
#include "MethodOverrideGraph.h"
#include "RedexTest.h"

class LoosenAccessModifierTest : public RedexTest {};

#define create_virtual_method(ident, scope, parent_type, access, name)      \
  auto ident = assembler::method_from_string("(method (" access ") \"" name \
                                             "\"((return-void)))");         \
  {                                                                         \
    ClassCreator cc(ident->get_class());                                    \
    cc.set_super(parent_type);                                              \
    cc.add_method(ident);                                                   \
    scope.push_back(cc.create());                                           \
  }

/**
 * a.A.bar() <- final a.A1.bar() -\- b.A11.bar()
 * b.A11.bar() does not override a.A1.bar() because of the visibility, so the
 * first two methods should not be public.
 */
TEST_F(LoosenAccessModifierTest, virtual_methods) {
  Scope scope;
  auto object = type::java_lang_Object();
  create_virtual_method(parent, scope, object, "", "La/A;.bar:()V");
  create_virtual_method(child, scope, parent->get_class(), "final",
                        "La/A1;.bar:()V");
  create_virtual_method(grand_child, scope, child->get_class(), "",
                        "Lb/A11;.bar:()V");

  EXPECT_FALSE(is_public(parent));
  EXPECT_FALSE(is_public(child));
  EXPECT_FALSE(is_public(grand_child));

  loosen_access_modifier(scope);

  EXPECT_FALSE(is_public(parent));
  EXPECT_FALSE(is_public(child));
  EXPECT_TRUE(is_public(grand_child));
}

#undef create_virtual_method
