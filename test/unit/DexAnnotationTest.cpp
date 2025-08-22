/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "RedexContext.h"
#include "RedexTest.h"

class DexAnnotationTest : public RedexTest {};

TEST_F(DexAnnotationTest, DeobfuscateStrings) {
  auto obj = DexType::make_type("Ljava/lang/Object;");
  auto a = DexType::make_type("LX/a;");
  DexType::make_type("[LX/a;");
  DexType::make_type("[[LX/a;");
  {
    ClassCreator creator(a);
    creator.set_super(obj);
    auto a_cls = creator.create();
    a_cls->set_deobfuscated_name(DexString::make_string("Lcom/fb/MyThing;"));
  }

  auto other = DexType::make_type("Lother/thing;");
  {
    ClassCreator creator(other);
    creator.set_super(obj);
    auto other_cls = creator.create();
    other_cls->set_deobfuscated_name(DexString::make_string("Lother/thing;"));
  }

  std::map<std::string, std::string> value_to_expected{
      {"LX/a;", "Lcom/fb/MyThing;"},
      {"LX/a<", "Lcom/fb/MyThing<"},
      {"Lother/thing;", "Lother/thing;"},
      {"[[LX/a;", "[[Lcom/fb/MyThing;"},
      {"no idea", "no idea"},
      {"", ""},
      {"[[[[[[[[[", "[[[[[[[[["}};

  for (auto&& [value, expected] : value_to_expected) {
    DexEncodedValueString ev(DexString::make_string(value));
    auto shown = ev.show_deobfuscated();
    EXPECT_STREQ(shown.c_str(), expected.c_str());
  }
}
