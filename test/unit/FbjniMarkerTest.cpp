/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexMemberRefs.h"
#include "FbjniMarker.h"
#include "RedexTest.h"

class FbjniMarkerTest : public RedexTest {};

// Test individual process function
TEST_F(FbjniMarkerTest, ProcessFunctionTest) {
  FbjniMarker marker;

  // initialize classes
  auto c1 = DexType::make_type("Ljava/lang/Object;");
  auto c2 = DexType::make_type("LFoo1;");

  ClassCreator cc1(c1);
  cc1.create();

  ClassCreator cc2(c2);
  cc2.set_super(c1);

  auto f1 = DexField::make_field("LFoo1;.f1:Ljava/lang/Object;")
                ->make_concrete(ACC_PUBLIC);
  cc2.add_field(f1);

  auto f2 = DexField::make_field("LFoo1;.f2:I")->make_concrete(ACC_PUBLIC);
  cc2.add_field(f2);

  auto f3 = DexField::make_field("LFoo1;.f3:[I")->make_concrete(ACC_PUBLIC);
  cc2.add_field(f3);

  auto f4 = DexField::make_field("LFoo1;.f4:[[Ljava/lang/Object;")
                ->make_concrete(ACC_PUBLIC);
  cc2.add_field(f4);

  auto m1 =
      DexMethod::make_method("LFoo1;.m1:()I")->make_concrete(ACC_PUBLIC, true);
  cc2.add_method(m1);

  auto m2 = DexMethod::make_method("LFoo1;.m2:(Ljava/lang/Object;B)V")

                ->make_concrete(ACC_PUBLIC, true);
  cc2.add_method(m2);

  auto m3 = DexMethod::make_method("LFoo1;.m3:(Ljava/lang/Object;[J)[C")
                ->make_concrete(ACC_PUBLIC, true);
  cc2.add_method(m3);

  auto init1 = DexMethod::make_method("LFoo1;.<init>:()V")
                   ->make_concrete(ACC_PUBLIC, true);
  cc2.add_method(init1);

  auto init2 = DexMethod::make_method("LFoo1;.<init>:([I)V")
                   ->make_concrete(ACC_PUBLIC, true);
  cc2.add_method(init2);

  cc2.create();

  // test process_class_path
  EXPECT_EQ(c1, marker.process_class_path("java.lang.Object"));
  EXPECT_EQ(c2, marker.process_class_path("Foo1"));

  // test process_field
  EXPECT_EQ(f1, marker.process_field(c2, "public Object f1;"));
  EXPECT_EQ(f2, marker.process_field(c2, "static final int f2;"));
  EXPECT_EQ(f3, marker.process_field(c2, "protected int[] f3;"));
  EXPECT_EQ(f4, marker.process_field(c2, "Object[][] f4;"));

  // test process_method
  EXPECT_EQ(m1, marker.process_method(c2, "int m1()"));
  EXPECT_EQ(
      m2,
      marker.process_method(
          c2, "private static void m2(Object a, byte b) throw Exception"));
  EXPECT_EQ(m3, marker.process_method(c2, "char[] m3(Object a, long[] b);"));

  // test constructor
  EXPECT_EQ(init1, marker.process_method(c2, "public Foo1()"));
  EXPECT_EQ(init2, marker.process_method(c2, "public Foo1(int[] a)"));
}

// Test in a more integrated way:
TEST_F(FbjniMarkerTest, FbjniJsonIntegTest) {
  // make type
  auto obj = DexType::make_type("Ljava/lang/Object;");
  auto cc0 = ClassCreator(obj);
  cc0.create();

  auto crash_log = DexType::make_type(
      "Lcom/facebook/common/dextricks/DalvikInternals$CrashLogParameters;");
  ClassCreator cc1(crash_log);
  cc1.set_super(obj);
  auto m = DexMethod::make_method(
               "Lcom/facebook/common/dextricks/"
               "DalvikInternals$CrashLogParameters;.getInstacrashInterval:()I")
               ->make_concrete(ACC_PUBLIC, true);
  cc1.add_method(m);
  cc1.create();

  auto adapter = DexType::make_type(
      "Lcom/facebook/livemaps/lens/data/parsing/model/RoomModelAdapter;");
  ClassCreator cc2(adapter);
  cc2.set_super(obj);
  auto f = DexField::make_field(
               "Lcom/facebook/livemaps/lens/data/parsing/model/"
               "RoomModelAdapter;.longitude:D")
               ->make_concrete(ACC_PUBLIC);
  cc2.add_field(f);
  cc2.create();

  auto exception = DexType::make_type("Ljava/lang/NullPointerException;");
  ClassCreator cc3(exception);
  cc3.set_super(obj);
  cc3.create();

  auto c1 = type_class(crash_log);
  ASSERT_NE(nullptr, c1);

  auto c2 = type_class(adapter);
  ASSERT_NE(nullptr, c2);

  auto c3 = type_class(exception);
  ASSERT_NE(nullptr, c3);

  // can rename before calling marker
  EXPECT_TRUE(c1->rstate.can_rename());
  EXPECT_TRUE(c2->rstate.can_rename());
  EXPECT_TRUE(c3->rstate.can_rename());
  EXPECT_TRUE(f->rstate.can_rename());
  EXPECT_TRUE(m->rstate.can_rename());

  // read json file and call marker
  std::vector<std::string> json_files;
  json_files.push_back(std::getenv("test_fbjni_json"));
  mark_native_classes_from_fbjni_configs(json_files);

  // test mark successfully
  EXPECT_FALSE(c1->rstate.can_rename());
  EXPECT_FALSE(c2->rstate.can_rename());
  EXPECT_FALSE(c3->rstate.can_rename());
  EXPECT_FALSE(f->rstate.can_rename());
  EXPECT_FALSE(m->rstate.can_rename());
}
