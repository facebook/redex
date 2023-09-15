/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HierarchyUtil.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "DexUtil.h"
#include "RedexTest.h"

using namespace hierarchy_util;

TEST_F(RedexTest, findNonOverriddenVirtuals) {
  // Begin creation of APK-internal class mock
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());

  auto final_method =
      DexMethod::make_method("LFoo;.final:()V")
          ->make_concrete(ACC_PUBLIC | ACC_FINAL, /* is_virtual */ true);
  cc.add_method(final_method);

  // This method is not explicitly marked as final, but no classes in scope
  // override its methods
  auto nonfinal_method = DexMethod::make_method("LFoo;.nonfinal:()V")
                             ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
  cc.add_method(nonfinal_method);
  auto cls = cc.create();

  // Begin creation of external class mock
  ClassCreator ext_cc(DexType::make_type("LExternal;"));
  ext_cc.set_super(type::java_lang_Object());
  ext_cc.set_external();

  auto ext_final_method =
      static_cast<DexMethod*>(DexMethod::make_method("LExternal;.final:()V"));
  ext_final_method->set_access(ACC_PUBLIC | ACC_FINAL);
  ext_final_method->set_virtual(true);
  ext_final_method->set_external();
  ext_cc.add_method(ext_final_method);

  // This method should not be included in the non-overridden set since it
  // could be overridden by some method we are not aware of.
  auto ext_nonfinal_method = static_cast<DexMethod*>(
      DexMethod::make_method("LExternal;.nonfinal:()V"));
  ext_nonfinal_method->set_access(ACC_PUBLIC);
  ext_nonfinal_method->set_virtual(true);
  ext_nonfinal_method->set_external();
  ext_cc.add_method(ext_nonfinal_method);

  ext_cc.create();

  NonOverriddenVirtuals non_overridden_virtuals({cls});
  std::unordered_set<DexMethod*> set;
  g_redex->walk_type_class([&](const DexType*, const DexClass* cls) {
    for (auto* method : cls->get_vmethods()) {
      if (non_overridden_virtuals.count(method)) {
        set.insert(method);
      }
    }
  });
  EXPECT_THAT(set,
              ::testing::UnorderedElementsAre(final_method, nonfinal_method,
                                              ext_final_method));
}
