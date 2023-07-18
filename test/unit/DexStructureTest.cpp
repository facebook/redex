/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStructure.h"
#include "Creators.h"
#include "DexUtil.h"
#include "RedexTest.h"
#include "VirtualScope.h"
#include <gtest/gtest.h>
#include <stdint.h>

class DexStructureTest : public RedexTest {};

DexClass* create_a_class(const char* description) {
  ClassCreator cc(DexType::make_type(description));
  cc.set_super(type::java_lang_Object());
  return cc.create();
}

TEST_F(DexStructureTest, remove_class) {
  DexStoresVector stores;
  auto foo_cls = create_a_class("Lfoo;");
  auto bar_cls = create_a_class("Lbar;");
  bar_cls->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);

  {
    DexStore store("root");
    store.add_classes({foo_cls, bar_cls});
    stores.push_back(std::move(store));
  }
  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);

  DexStructure dex1;

  DexType* ty = DexType::make_type("Lbaz;");
  DexProto* proto = DexProto::make_proto(ty, DexTypeList::make_type_list({}));
  auto str1 = DexString::make_string("m1");
  auto str2 = DexString::make_string("m2");
  auto str3 = DexString::make_string("m3");
  DexMethodRef* m1 = DexMethod::make_method(ty, str1, proto);
  DexMethodRef* m2 = DexMethod::make_method(ty, str2, proto);
  DexMethodRef* m3 = DexMethod::make_method(ty, str3, proto);
  DexFieldRef* f1 = DexField::make_field(ty, DexString::make_string("f1"), ty);
  DexFieldRef* f2 = DexField::make_field(ty, DexString::make_string("f2"), ty);
  DexFieldRef* f3 = DexField::make_field(ty, DexString::make_string("f3"), ty);

  dex1.add_class_no_checks({m1, m2}, {f2, f3}, {ty}, {}, {}, 0, foo_cls);
  dex1.add_class_no_checks({m1, m2, m3}, {f1}, {ty}, {}, {}, 0, bar_cls);
  EXPECT_EQ(dex1.get_mref_occurrences(m1), 2);
  EXPECT_EQ(dex1.get_mref_occurrences(m2), 2);
  EXPECT_EQ(dex1.get_mref_occurrences(m3), 1);

  EXPECT_EQ(dex1.get_fref_occurrences(f1), 1);
  EXPECT_EQ(dex1.get_fref_occurrences(f2), 1);
  EXPECT_EQ(dex1.get_fref_occurrences(f3), 1);

  EXPECT_EQ(dex1.get_tref_occurrences(ty), 2);

  // Check perf based classes sorting.
  auto classes = dex1.get_classes();
  EXPECT_EQ(classes.size(), 2);
  EXPECT_EQ(classes[0]->is_perf_sensitive(), false);
  EXPECT_EQ(classes[1]->is_perf_sensitive(), true);

  classes = dex1.get_classes(true);
  EXPECT_EQ(classes[0]->is_perf_sensitive(), true);
  EXPECT_EQ(classes[1]->is_perf_sensitive(), false);

  // Remove foo1_cls.
  dex1.remove_class(&init_classes_with_side_effects, {m1}, {f2}, {ty}, {}, {},
                    0, foo_cls);
  EXPECT_EQ(dex1.get_mref_occurrences(m1), 1);
  EXPECT_EQ(dex1.get_mref_occurrences(m2), 2);
  EXPECT_EQ(dex1.get_mref_occurrences(m3), 1);
  EXPECT_EQ(dex1.get_fref_occurrences(f1), 1);
  EXPECT_EQ(dex1.get_fref_occurrences(f2), 0);
  EXPECT_EQ(dex1.get_fref_occurrences(f3), 1);
  EXPECT_EQ(dex1.get_tref_occurrences(ty), 1);
  EXPECT_EQ(dex1.size(), 1);
}
