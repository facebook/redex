/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexUtil.h"
#include "RedexTest.h"
#include <gtest/gtest.h>

class DexUtilTest : public RedexTest {};

TEST_F(DexUtilTest, test_java_name_internal_to_external) {
  using namespace java_names;
  EXPECT_EQ("java.lang.String", internal_to_external("Ljava/lang/String;"));
  EXPECT_EQ("[Ljava.lang.String;", internal_to_external("[Ljava/lang/String;"));
  EXPECT_EQ("[[Ljava.lang.String;",
            internal_to_external("[[Ljava/lang/String;"));
  EXPECT_EQ("int", internal_to_external("I"));
  EXPECT_EQ("[I", internal_to_external("[I"));
  EXPECT_EQ("[[I", internal_to_external("[[I"));
  EXPECT_EQ("MyClass", internal_to_external("LMyClass;"));
  EXPECT_EQ("[LMyClass;", internal_to_external("[LMyClass;"));
  EXPECT_EQ("[[LMyClass;", internal_to_external("[[LMyClass;"));
}

TEST_F(DexUtilTest, test_java_name_external_to_internal) {
  using namespace java_names;
  EXPECT_EQ("Ljava/lang/String;", external_to_internal("java.lang.String"));
  EXPECT_EQ("[Ljava/lang/String;", external_to_internal("[Ljava.lang.String;"));
  EXPECT_EQ("[[Ljava/lang/String;",
            external_to_internal("[[Ljava.lang.String;"));

  EXPECT_EQ("I", external_to_internal("int"));
  EXPECT_EQ("LI;", external_to_internal("I"));
  EXPECT_EQ("[I", external_to_internal("[I"));
  EXPECT_EQ("[[I", external_to_internal("[[I"));
  EXPECT_EQ("[[LI;", external_to_internal("[[LI;"));

  EXPECT_EQ("LMyClass;", external_to_internal("MyClass"));
  EXPECT_EQ("[LMyClass;", external_to_internal("[LMyClass;"));
  EXPECT_EQ("[[LMyClass;", external_to_internal("[[LMyClass;"));
  EXPECT_EQ("L;", external_to_internal(""));
  EXPECT_EQ("[[;", external_to_internal("[["));
}

TEST_F(DexUtilTest, test_java_name_internal_to_simple) {
  using namespace java_names;
  EXPECT_EQ("String", internal_to_simple("Ljava/lang/String;"));
  EXPECT_EQ("String[]", internal_to_simple("[Ljava/lang/String;"));
  EXPECT_EQ("String[][]", internal_to_simple("[[Ljava/lang/String;"));
  EXPECT_EQ("int", internal_to_simple("I"));
  EXPECT_EQ("int[]", internal_to_simple("[I"));
  EXPECT_EQ("int[][]", internal_to_simple("[[I"));
  EXPECT_EQ("MyClass", internal_to_simple("LMyClass;"));
  EXPECT_EQ("MyClass[]", internal_to_simple("[LMyClass;"));
  EXPECT_EQ("MyClass[][]", internal_to_simple("[[LMyClass;"));
}

TEST_F(DexUtilTest, is_valid_identifier) {
  EXPECT_TRUE(is_valid_identifier("FooBar123$Hello_World-Test"));

  // TODO: Add support for UTF.
  // TODO: Add support for different dex versions.

  EXPECT_FALSE(is_valid_identifier("[Foo"));
  EXPECT_FALSE(is_valid_identifier("Foo;"));
  EXPECT_FALSE(is_valid_identifier("foo.bar"));
  EXPECT_FALSE(is_valid_identifier("foo/bar"));
}

TEST_F(DexUtilTest, is_valid_identifier_range) {
  std::string s = ";[FooBar123$Hello_World-Test./";
  EXPECT_TRUE(is_valid_identifier(s, 2, s.length() - 4));

  EXPECT_FALSE(is_valid_identifier(s, 1, s.length() - 4));
  EXPECT_FALSE(is_valid_identifier(s, 2, s.length() - 3));

  EXPECT_FALSE(is_valid_identifier(s, 2, 0));

  std::string mod = s;
  mod[mod.length() / 2] = ';';
  EXPECT_FALSE(is_valid_identifier(mod, 2, mod.length() - 4));
}
