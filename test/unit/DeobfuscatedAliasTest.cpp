/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"

#include "RedexTest.h"
#include <gtest/gtest.h>

/*Testing getting dex fields & methods with a deobfuscated name*/
class DeobfuscatedAliasTest : public RedexTest {};

// DexField

TEST_F(DeobfuscatedAliasTest, testField) {
  auto field = DexField::make_field("Lbaz;.foo:I")->make_concrete(ACC_PUBLIC);
  field->set_deobfuscated_name("qux");
  field->set_deobfuscated_name("bar");
  auto get_result_original = DexField::get_field("Lbaz;.foo:I");
  auto get_result_old_deobfuscated = DexField::get_field("Lbaz;.qux:I");
  auto get_result_deobfuscated = DexField::get_field("Lbaz;.bar:I");
  EXPECT_EQ(field, get_result_original);
  EXPECT_EQ(nullptr, get_result_old_deobfuscated);
  EXPECT_EQ("bar", field->get_deobfuscated_name_or_empty());
  EXPECT_EQ(field, get_result_deobfuscated);
}

TEST_F(DeobfuscatedAliasTest, testFieldDuplicate) {
  auto field = DexField::make_field("Lbaz;.foo:I")->make_concrete(ACC_PUBLIC);
  field->set_deobfuscated_name("foo");
  // Doesn't change anything, but shouldn't crash
  auto get_result = DexField::get_field("Lbaz;.foo:I");
  EXPECT_EQ("foo", field->get_deobfuscated_name_or_empty());
  EXPECT_EQ(field, get_result);
}

TEST_F(DeobfuscatedAliasTest, testFieldExisting) {
  auto field = DexField::make_field("Lbaz;.foo:I")->make_concrete(ACC_PUBLIC);
  auto field2 = DexField::make_field("Lbaz;.bar:I")->make_concrete(ACC_PUBLIC);
  field->set_deobfuscated_name("bar");
  field2->set_deobfuscated_name("foo");
  // Doesn't change anything, but shouldn't crash
  auto get_result = DexField::get_field("Lbaz;.foo:I");
  auto get_result2 = DexField::get_field("Lbaz;.bar:I");
  EXPECT_EQ(field, get_result);
  EXPECT_EQ(field2, get_result2);
}

// Dex Method

TEST_F(DeobfuscatedAliasTest, testMethod) {
  auto method =
      DexMethod::make_method("Lbaz;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  method->set_deobfuscated_name("qux");
  method->set_deobfuscated_name("bar");
  auto get_result_original = DexMethod::get_method("Lbaz;.foo:()I");
  auto get_result_old_deobfuscated = DexMethod::get_method("Lbaz;.qux:()I");
  auto get_result_deobfuscated = DexMethod::get_method("Lbaz;.bar:()I");
  EXPECT_EQ(method, get_result_original);
  EXPECT_EQ(nullptr, get_result_old_deobfuscated);
  EXPECT_EQ("bar", method->get_deobfuscated_name_or_empty());
  EXPECT_EQ(method, get_result_deobfuscated);
}

TEST_F(DeobfuscatedAliasTest, testMethodDuplicate) {
  auto method =
      DexMethod::make_method("Lbaz;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  method->set_deobfuscated_name("foo");
  // Doesn't change anything, but shouldn't crash
  auto get_result = DexMethod::get_method("Lbaz;.foo:()I");
  EXPECT_EQ("foo", method->get_deobfuscated_name_or_empty());
  EXPECT_EQ(method, get_result);
}

TEST_F(DeobfuscatedAliasTest, testMethodExisting) {
  auto method =
      DexMethod::make_method("Lbaz;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  auto method2 =
      DexMethod::make_method("Lbaz;.bar:()I")->make_concrete(ACC_PUBLIC, true);
  method->set_deobfuscated_name("bar");
  method2->set_deobfuscated_name("foo");
  // Doesn't change anything, but shouldn't crash
  auto get_result = DexMethod::get_method("Lbaz;.foo:()I");
  auto get_result2 = DexMethod::get_method("Lbaz;.bar:()I");
  EXPECT_EQ(method, get_result);
  EXPECT_EQ(method2, get_result2);
}
