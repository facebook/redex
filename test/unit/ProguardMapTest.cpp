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
    "    java.lang.String ƒKEY_FILTER -> ƒKEY_FILTER\n"
    "    3:3:void <init>() -> <init>\n"
    "    8:929:java.util.ArrayList getCopy() -> a\n"
    "    1807:1807:android.content.Context android.support.v7.view.menu.MenuBuilder.getContext():807:807 -> addSubMenu\n"
    "android.support.v4.app.Fragment -> android.support.v4.app.Fragment:\n"
    "    android.support.v4.util.SimpleArrayMap sClassMap -> sClassMap\n"
    "    1:10:com.foo.bar stuff(com.foo.bar,com.foo.bar) -> x\n"
    "android.support.v4.util.SimpleArrayMap -> android.support.v4.b.b:\n"
    "com.instagram.common.api.base.Header -> com.instagram.common.j.a.f:\n"
    "com.facebook.react.bridge.WritableMap -> com.facebook.react.bridge.e:\n"
    "com.instagram.react.IgNetworkingModule -> com.instagram.react.IgNetworkingModule:\n"
    "    356:368:com.facebook.react.bridge.WritableMap translateHeaders(com.instagram.common.api.base.Header[]) -> translateHeaders\n"
  );
  ProguardMap pm(ss);
  EXPECT_EQ("LA;", pm.translate_class("Lcom/foo/bar;"));
  EXPECT_EQ("LA;.a:I", pm.translate_field("Lcom/foo/bar;.do1:I"));
  EXPECT_EQ("LA;.<init>:()V", pm.translate_method("Lcom/foo/bar;.<init>:()V"));
  EXPECT_EQ("LA;.a:()Ljava/util/ArrayList;", pm.translate_method("Lcom/foo/bar;.getCopy:()Ljava/util/ArrayList;"));
  EXPECT_EQ("Lcom/not/Found;", pm.translate_class("Lcom/not/Found;"));
  EXPECT_EQ("Landroid/support/v4/b/b;", pm.translate_class("Landroid/support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ("Landroid/support/v4/app/Fragment;.sClassMap:Landroid/support/v4/b/b;", pm.translate_field("Landroid/support/v4/app/Fragment;.sClassMap:Landroid/support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ("Landroid/support/v4/app/Fragment;.x:(LA;LA;)LA;", pm.translate_method("Landroid/support/v4/app/Fragment;.stuff:(Lcom/foo/bar;Lcom/foo/bar;)Lcom/foo/bar;"));
  EXPECT_EQ("Lcom/instagram/react/IgNetworkingModule;.translateHeaders:([Lcom/instagram/common/j/a/f;)Lcom/facebook/react/bridge/e;", pm.translate_method("Lcom/instagram/react/IgNetworkingModule;.translateHeaders:([Lcom/instagram/common/api/base/Header;)Lcom/facebook/react/bridge/WritableMap;"));
}
