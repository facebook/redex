/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"
#include "IRAssembler.h"
#include "RedexTest.h"

class DexClassTest : public RedexTest {};

TEST_F(DexClassTest, testUniqueMethodName) {
  auto method = assembler::class_with_method("LFoo;",
                                             R"(
      (method (private) "LFoo;.bar:(I)V"
       (
        (return-void)
       )
      )
    )");
  auto type = DexType::make_type("LFoo;");
  DexString* newname = DexMethod::get_unique_name(
      type, DexString::make_string("bar"), method->get_proto());
  EXPECT_EQ(newname->str(), "barr$0");
  DexMethod::make_method("LFoo;.barr$0:(I)V");
  newname = DexMethod::get_unique_name(
      type, DexString::make_string("bar"), method->get_proto());
  EXPECT_EQ(newname->str(), "barr$1");

  newname = DexMethod::get_unique_name(
      type, DexString::make_string("baz"), method->get_proto());
  EXPECT_EQ(newname->str(),
            "baz"); // no conflict, expect baz not to be suffixed
}

TEST_F(DexClassTest, testUniqueFieldName) {
  auto class_type = DexType::make_type(DexString::make_string("LFoo;"));
  ClassCreator class_creator(class_type);
  class_creator.set_super(type::java_lang_Object());
  class_creator.create();
  auto type = DexType::make_type("LFoo;");
  DexString* newname = DexField::get_unique_name(
      type, DexString::make_string("bar"), DexType::make_type("I"));
  EXPECT_EQ(newname->str(), "bar"); // no conflict, should not be renamed
  DexField::make_field("LFoo;.bar:I");
  newname = DexField::get_unique_name(
      type, DexString::make_string("bar"), DexType::make_type("I"));
  EXPECT_EQ(newname->str(), "barr$0");
  DexField::make_field("LFoo;.barr$0:I");
  newname = DexField::get_unique_name(
      type, DexString::make_string("bar"), DexType::make_type("I"));
  EXPECT_EQ(newname->str(), "barr$1");
}

TEST_F(DexClassTest, testUniqueTypeName) {
  DexType::make_type("LFoo;");
  auto bar_type = DexType::make_unique_type("LBar;");
  auto foor0_type = DexType::make_unique_type("LFoo;");
  auto foor1_type = DexType::make_unique_type("LFoo;");
  auto foor0r0_type = DexType::make_unique_type("LFoor$0;");

  EXPECT_EQ(bar_type->str(), "LBar;"); // no conflict, should not be renamed
  EXPECT_EQ(foor0_type->str(), "LFoor$0;");
  EXPECT_EQ(foor1_type->str(), "LFoor$1;");
  EXPECT_EQ(foor0r0_type->str(), "LFoor$0r$0;");
}
