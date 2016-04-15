/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <sstream>

#include "ProguardMap.h"

TEST(ProguardMapTest, empty) {
  std::stringstream ss(
    "com.foo.bar -> A:\n"
    "    int do1 -> a\n"
    "    3:3:void <init>() -> <init>\n"
    "    8:929:java.util.ArrayList getCopy() -> a\n"
    "    1807:1807:android.content.Context android.support.v7.view.menu.MenuBuilder.getContext():807:807 -> addSubMenu\n"
  );
  ProguardMap pm(ss);
  EXPECT_EQ(pm.translate_class("Lcom/foo/bar;"), "LA;");
  EXPECT_EQ(pm.translate_field("Lcom/foo/bar;.do1:I"), "LA;.a:I");
  EXPECT_EQ(pm.translate_method("Lcom/foo/bar;.<init>()V"), "LA;.<init>()V");
  EXPECT_EQ(pm.translate_method("Lcom/foo/bar;.getCopy()Ljava/util/ArrayList;"), "LA;.a()Ljava/util/ArrayList;");
  EXPECT_EQ(pm.translate_class("Lcom/not/Found;"), "Lcom/not/Found;");
}
