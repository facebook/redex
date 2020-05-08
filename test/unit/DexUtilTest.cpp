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

TEST_F(DexUtilTest, test_reference_type_wrappers) {
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("Z")),
            DexType::make_type("Ljava/lang/Boolean;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("B")),
            DexType::make_type("Ljava/lang/Byte;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("S")),
            DexType::make_type("Ljava/lang/Short;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("C")),
            DexType::make_type("Ljava/lang/Character;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("I")),
            DexType::make_type("Ljava/lang/Integer;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("J")),
            DexType::make_type("Ljava/lang/Long;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("F")),
            DexType::make_type("Ljava/lang/Float;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("D")),
            DexType::make_type("Ljava/lang/Double;"));
}

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
