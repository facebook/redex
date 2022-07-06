/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

namespace {

DexMethod* find_dmethod(const DexClass& cls,
                        const std::string& name,
                        const std::string& proto) {
  auto dmethods = cls.get_dmethods();
  auto it = std::find_if(
      dmethods.begin(), dmethods.end(), [&name, &proto](DexMethod* m) {
        return m->get_name()->str() == name && proto == show(m->get_proto());
      });
  return it == dmethods.end() ? nullptr : *it;
}

// Check argument reordering
TEST_F(PostVerify, Reorderables) {
  auto cls1 = find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo;");
  ASSERT_NE(nullptr, cls1);
  auto cls2 = find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar;");
  ASSERT_NE(nullptr, cls1);

  auto foo = find_dmethod(*cls1, "<init>", "()V");
  ASSERT_NE(nullptr, foo);

  auto bar = find_dmethod(*cls2, "<init>", "(I)V");
  ASSERT_NE(nullptr, bar);
}
} // namespace
