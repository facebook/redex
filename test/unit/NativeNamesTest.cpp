/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "NativeNames.h"
#include "RedexTest.h"

using namespace native_names;

struct NativeNamesTest : public RedexTest {};

TEST_F(NativeNamesTest, testBasicMethodNames) {
  auto m = DexMethod::make_method(
      "Lcom/facebook/redex/SomeClass;.someMethod:(ILjava/lang/String;)Ljava/"
      "lang/Object;");
  EXPECT_EQ("Java_com_facebook_redex_SomeClass_someMethod",
            get_native_short_name_for_method(m));
  EXPECT_EQ(
      "Java_com_facebook_redex_SomeClass_someMethod__ILjava_lang_String_2",
      get_native_long_name_for_method(m));
}

TEST_F(NativeNamesTest, testEscaping) {
  auto m = DexMethod::make_method(
      "Lcom/facebook/redex/Some$Interesting_Class;.$ome_Method:(ILjava/lang/"
      "Str_ing;)Ljava/lang/Ob$ect;");
  EXPECT_EQ(
      "Java_com_facebook_redex_Some_00024Interesting_1Class__00024ome_1Method",
      get_native_short_name_for_method(m));
  EXPECT_EQ(
      "Java_com_facebook_redex_Some_00024Interesting_1Class__00024ome_1Method__"
      "ILjava_lang_Str_1ing_2",
      get_native_long_name_for_method(m));
}
