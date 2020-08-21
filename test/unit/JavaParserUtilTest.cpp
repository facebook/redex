/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JavaParserUtil.h"
#include "RedexTest.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class JavaParserUtilTest : public RedexTest {};

using namespace java_declarations;

TEST_F(JavaParserUtilTest, test_parse_field) {
  std::string str1 = "int a;";
  dex_member_refs::FieldDescriptorTokens fdt1 = parse_field_declaration(str1);
  EXPECT_EQ("int", fdt1.type);
  EXPECT_EQ("a", fdt1.name);

  std::string str2 = "private float b";
  dex_member_refs::FieldDescriptorTokens fdt2 = parse_field_declaration(str2);
  EXPECT_EQ("float", fdt2.type);
  EXPECT_EQ("b", fdt2.name);

  std::string str3 = "static final Object[]    c;  ";
  dex_member_refs::FieldDescriptorTokens fdt3 = parse_field_declaration(str3);
  EXPECT_EQ("Object[]", fdt3.type);
  EXPECT_EQ("c", fdt3.name);

  std::string str4 = "com.facebook.util.MyClass d;";
  dex_member_refs::FieldDescriptorTokens fdt4 = parse_field_declaration(str4);
  EXPECT_EQ("com.facebook.util.MyClass", fdt4.type);
  EXPECT_EQ("d", fdt4.name);
}

TEST_F(JavaParserUtilTest, test_parse_method) {
  // test normal method
  std::string str1 = "public static void main(String[] args)";
  dex_member_refs::MethodDescriptorTokens mdt1 = parse_method_declaration(str1);
  EXPECT_EQ("void", mdt1.rtype);
  EXPECT_EQ("main", mdt1.name);
  EXPECT_THAT(mdt1.args, ::testing::ElementsAre("String[]"));

  std::string str2 = "int a()";
  dex_member_refs::MethodDescriptorTokens mdt2 = parse_method_declaration(str2);
  EXPECT_EQ("int", mdt2.rtype);
  EXPECT_EQ("a", mdt2.name);
  EXPECT_THAT(mdt2.args, ::testing::ElementsAre());

  std::string str3 = "private synchronized Object b(String x, int y)";
  dex_member_refs::MethodDescriptorTokens mdt3 = parse_method_declaration(str3);
  EXPECT_EQ("Object", mdt3.rtype);
  EXPECT_EQ("b", mdt3.name);
  EXPECT_THAT(mdt3.args, ::testing::ElementsAre("String", "int"));

  // test constructor
  std::string str4 = "public Bar(double[] x)";
  dex_member_refs::MethodDescriptorTokens mdt4 = parse_method_declaration(str4);
  EXPECT_EQ(std::string(), mdt4.rtype);
  EXPECT_EQ("Bar", mdt4.name);
  EXPECT_THAT(mdt4.args, ::testing::ElementsAre("double[]"));
}
