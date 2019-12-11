/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardMap.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>

#include "IRAssembler.h"
#include "RedexTest.h"

using ::testing::AllOf;
using ::testing::Pointee;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

class ProguardMapTest : public RedexTest {};

TEST_F(ProguardMapTest, empty) {
  std::stringstream ss(
      "com.foo.bar -> A:\n"
      "    int do1 -> a\n"
      "    java.lang.String ƒKEY_FILTER -> ƒKEY_FILTER\n"
      "    3:3:void <init>() -> <init>\n"
      "    8:929:java.util.ArrayList getCopy() -> a\n"
      "    1807:1807:android.content.Context "
      "android.support.v7.view.menu.MenuBuilder.getContext():807:807 -> "
      "addSubMenu\n"
      "android.support.v4.app.Fragment -> android.support.v4.app.Fragment:\n"
      "    android.support.v4.util.SimpleArrayMap sClassMap -> sClassMap\n"
      "    1:10:com.foo.bar stuff(com.foo.bar,com.foo.bar) -> x\n"
      "android.support.v4.util.SimpleArrayMap -> android.support.v4.b.b:\n"
      "com.instagram.common.api.base.Header -> com.instagram.common.j.a.f:\n"
      "com.facebook.react.bridge.WritableMap -> com.facebook.react.bridge.e:\n"
      "com.instagram.react.IgNetworkingModule -> "
      "com.instagram.react.IgNetworkingModule:\n"
      "    a_vcard.android.syncml.pim.VBuilder mExecutorSupplier$7ec36e13 -> "
      "b\n"
      "    356:368:com.facebook.react.bridge.WritableMap "
      "translateHeaders(com.instagram.common.api.base.Header[]) -> "
      "translateHeaders\n");
  ProguardMap pm(ss);
  EXPECT_EQ("LA;", pm.translate_class("Lcom/foo/bar;"));
  EXPECT_EQ("LA;.a:I", pm.translate_field("Lcom/foo/bar;.do1:I"));
  EXPECT_EQ("LA;.<init>:()V", pm.translate_method("Lcom/foo/bar;.<init>:()V"));
  EXPECT_EQ(
      "LA;.a:()Ljava/util/ArrayList;",
      pm.translate_method("Lcom/foo/bar;.getCopy:()Ljava/util/ArrayList;"));
  EXPECT_EQ("LA;.addSubMenu:()Landroid/content/Context;",
            pm.translate_method(
                "Landroid/support/v7/view/menu/"
                "MenuBuilder;.getContext:()Landroid/content/Context;"));
  EXPECT_EQ("Lcom/not/Found;", pm.translate_class("Lcom/not/Found;"));
  EXPECT_EQ("Landroid/support/v4/b/b;",
            pm.translate_class("Landroid/support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ(
      "Landroid/support/v4/app/Fragment;.sClassMap:Landroid/support/v4/b/b;",
      pm.translate_field("Landroid/support/v4/app/Fragment;.sClassMap:Landroid/"
                         "support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ("Landroid/support/v4/app/Fragment;.x:(LA;LA;)LA;",
            pm.translate_method("Landroid/support/v4/app/Fragment;.stuff:(Lcom/"
                                "foo/bar;Lcom/foo/bar;)Lcom/foo/bar;"));
  EXPECT_EQ(
      "Lcom/instagram/react/IgNetworkingModule;.translateHeaders:([Lcom/"
      "instagram/common/j/a/f;)Lcom/facebook/react/bridge/e;",
      pm.translate_method(
          "Lcom/instagram/react/IgNetworkingModule;.translateHeaders:([Lcom/"
          "instagram/common/api/base/Header;)Lcom/facebook/react/bridge/"
          "WritableMap;"));
  EXPECT_EQ(true,
            pm.is_special_interface("La_vcard/android/syncml/pim/VBuilder;"));
  EXPECT_EQ(false, pm.is_special_interface("Lcom/not/Found;"));
}

TEST_F(ProguardMapTest, HandlesGeneratedComments) {
  std::stringstream ss(
      "# compiler: R8\n"
      "# compiler_version: 1.3.23\n"
      "# min_api: 15\n"
      "com.foo.bar -> A:\n"
      "    int do1 -> a\n");
  ProguardMap pm(ss);
  EXPECT_EQ("LA;", pm.translate_class("Lcom/foo/bar;"));
  EXPECT_EQ("LA;.a:I", pm.translate_field("Lcom/foo/bar;.do1:I"));
}

TEST_F(ProguardMapTest, LineNumbers) {
  std::stringstream ss(
      "com.foo.bar -> A:\n"
      "    int do1 -> a\n"
      "    3:3:void <init>() -> <init>\n"
      "    3:3:void <init>() -> <init>\n"
      "    java.io.File createTempFile() -> a\n"
      "    3:void stuff() -> b\n"
      "    1:1:boolean isExpired():490:490 -> k\n"
      "    1:1:boolean isRequirementsMet():275 -> k\n"
      "    2:2:long com.whatsapp.core.Time.currentServerTimeMillis():66:66 -> "
      "k\n"
      "    2:2:boolean isExpired():490 -> k\n"
      "    2:2:boolean isRequirementsMet():275 -> k\n"
      "    3:3:boolean isExpired():491:491 -> k\n"
      "    3:3:boolean isRequirementsMet():275 -> k\n"
      "    4:4:boolean isRequirementsMet():275:275 -> k\n"
      "    1:2:void onRun():282:283 -> o\n"
      "    3:3:void onRun():385:385 -> o\n"
      "    4:5:void onRun():286:287 -> o\n"
      "    6:6:void onRun():289:289 -> o\n"
      "    7:7:void onRun():382:382 -> o\n"
      "    8:8:void onRun():385:385 -> o\n"
      "    9:9:void onRun():387:387 -> o\n"
      "com.foo.Inline -> B:\n"
      "    1000:1001:void bar():1 -> a\n"
      "    1000:1001:void baz():1 -> a\n"
      "android.support.v4.app.Fragment -> android.support.v4.app.Fragment:\n"
      "    android.support.v4.util.SimpleArrayMap sClassMap -> sClassMap\n"
      "    1:10:com.foo.bar stuff(com.foo.bar,com.foo.bar) -> o\n"
      "android.support.v4.util.SimpleArrayMap -> android.support.v4.b.b:\n");
  ProguardMap pm(ss);
  EXPECT_EQ("LA;", pm.translate_class("Lcom/foo/bar;"));
  EXPECT_EQ("LA;.a:I", pm.translate_field("Lcom/foo/bar;.do1:I"));
  EXPECT_EQ("LA;.<init>:()V", pm.translate_method("Lcom/foo/bar;.<init>:()V"));
  EXPECT_EQ(
      "LA;.a:()Ljava/io/File;",
      pm.translate_method("Lcom/foo/bar;.createTempFile:()Ljava/io/File;"));
  EXPECT_EQ("LA;.b:()V", pm.translate_method("Lcom/foo/bar;.stuff:()V"));
  EXPECT_EQ("LA;.k:()Z", pm.translate_method("Lcom/foo/bar;.isExpired:()Z"));
  EXPECT_EQ("LA;.k:()Z",
            pm.translate_method("Lcom/foo/bar;.isRequirementsMet:()Z"));

  EXPECT_EQ("LA;.k:()J",
            pm.translate_method(
                "Lcom/whatsapp/core/Time;.currentServerTimeMillis:()J"));
  EXPECT_EQ("LA;.o:()V", pm.translate_method("Lcom/foo/bar;.onRun:()V"));
  EXPECT_EQ("Landroid/support/v4/b/b;",
            pm.translate_class("Landroid/support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ(
      "Landroid/support/v4/app/Fragment;.sClassMap:Landroid/support/v4/b/b;",
      pm.translate_field("Landroid/support/v4/app/Fragment;.sClassMap:Landroid/"
                         "support/v4/util/SimpleArrayMap;"));
  EXPECT_EQ("Landroid/support/v4/app/Fragment;.o:(LA;LA;)LA;",
            pm.translate_method("Landroid/support/v4/app/Fragment;.stuff:(Lcom/"
                                "foo/bar;Lcom/foo/bar;)Lcom/foo/bar;"));

  EXPECT_THAT(pm.method_lines("LA;.<init>:()V"),
              AllOf(SizeIs(2),
                    UnorderedElementsAre(
                        Pointee(ProguardLineRange(3, 3, 0, 0,
                                                  "Lcom/foo/bar;.<init>:()V")),
                        Pointee(ProguardLineRange(
                            3, 3, 0, 0, "Lcom/foo/bar;.<init>:()V")))));
  EXPECT_THAT(pm.method_lines("LA;.a:()Ljava/io/File;"),
              AllOf(SizeIs(1),
                    UnorderedElementsAre(Pointee(ProguardLineRange(
                        0, 0, 0, 0,
                        "Lcom/foo/bar;.createTempFile:()Ljava/io/File;")))));
  EXPECT_THAT(pm.method_lines("LA;.b:()V"),
              AllOf(SizeIs(1),
                    UnorderedElementsAre(Pointee(ProguardLineRange(
                        3, 0, 0, 0, "Lcom/foo/bar;.stuff:()V")))));
  auto expected = AllOf(
      SizeIs(8),
      UnorderedElementsAre(
          Pointee(
              ProguardLineRange(1, 1, 490, 490, "Lcom/foo/bar;.isExpired:()Z")),
          Pointee(ProguardLineRange(1, 1, 275, 0,
                                    "Lcom/foo/bar;.isRequirementsMet:()Z")),
          Pointee(
              ProguardLineRange(2, 2, 490, 0, "Lcom/foo/bar;.isExpired:()Z")),
          Pointee(ProguardLineRange(2, 2, 275, 0,
                                    "Lcom/foo/bar;.isRequirementsMet:()Z")),
          Pointee(
              ProguardLineRange(3, 3, 491, 491, "Lcom/foo/bar;.isExpired:()Z")),
          Pointee(ProguardLineRange(3, 3, 275, 0,
                                    "Lcom/foo/bar;.isRequirementsMet:()Z")),
          Pointee(ProguardLineRange(4, 4, 275, 275,
                                    "Lcom/foo/bar;.isRequirementsMet:()Z")),
          Pointee(ProguardLineRange(
              2, 2, 66, 66,
              "Lcom/whatsapp/core/Time;.currentServerTimeMillis:()J"))));

  EXPECT_THAT(pm.method_lines("LA;.k:()Z"), expected);
  EXPECT_THAT(pm.method_lines("LA;.k:()J"), expected);

  EXPECT_THAT(pm.method_lines("LA;.o:()V"),
              AllOf(SizeIs(7),
                    UnorderedElementsAre(
                        Pointee(ProguardLineRange(1, 2, 282, 283,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(3, 3, 385, 385,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(4, 5, 286, 287,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(6, 6, 289, 289,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(7, 7, 382, 382,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(8, 8, 385, 385,
                                                  "Lcom/foo/bar;.onRun:()V")),
                        Pointee(ProguardLineRange(
                            9, 9, 387, 387, "Lcom/foo/bar;.onRun:()V")))));
  EXPECT_THAT(pm.method_lines("LB;.a:()V"),
              AllOf(SizeIs(2),
                    UnorderedElementsAre(
                        Pointee(ProguardLineRange(1000, 1001, 1, 0,
                                                  "Lcom/foo/Inline;.bar:()V")),
                        Pointee(ProguardLineRange(
                            1000, 1001, 1, 0, "Lcom/foo/Inline;.baz:()V")))));
  EXPECT_THAT(
      pm.method_lines("Landroid/support/v4/app/Fragment;.o:(LA;LA;)LA;"),
      AllOf(SizeIs(1),
            UnorderedElementsAre(Pointee(ProguardLineRange(
                1, 10, 0, 0,
                "Landroid/support/v4/app/Fragment;.stuff:(Lcom/"
                "foo/bar;Lcom/foo/bar;)Lcom/foo/bar;")))));
}

TEST_F(ProguardMapTest, LinesKey) {
  EXPECT_EQ("LA;.o", pg_impl::lines_key("LA;.o:()V"));
  EXPECT_EQ(
      "Landroid/support/v4/app/Fragment;.o",
      pg_impl::lines_key("Landroid/support/v4/app/Fragment;.o:(LA;LA;)LA;"));
}

TEST_F(ProguardMapTest, FileNameFromMethodString) {

  {
    auto method_string = DexString::make_string(
        "Landroid/support/v4/app/Fragment;.stuff:(Lcom/foo/bar;Lcom/foo/"
        "bar;)Lcom/foo/bar;");
    EXPECT_EQ(pg_impl::file_name_from_method_string(method_string),
              DexString::make_string("Fragment.java"));
  }
  {
    auto method_string =
        DexString::make_string("Lcom/foo/Bar$Inner;.stuff:()V");
    EXPECT_EQ(pg_impl::file_name_from_method_string(method_string),
              DexString::make_string("Bar.java"));
  }
}

TEST_F(ProguardMapTest, DeobfuscateFrameWithRelocation) {

  std::stringstream ss(
      "com.foo.Bar -> X.A:\n"
      "    short com.blah.foo.bar.boo(byte) -> a\n"
      "    2:2:long com.whatsapp.core.Time.currentServerTimeMillis():66:66 -> "
      "a\n");

  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "LX/A;.a:()J" "SourceFile" 2)
      (const v1 0)
      (return-void)
    )
)");

  ProguardMap pm(ss);
  pg_impl::apply_deobfuscated_positions(code.get(), pm);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "Lcom/whatsapp/core/Time;.currentServerTimeMillis:()J" Time.java 66)
      (const v1 0)
      (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ProguardMapTest, DeobfuscateFramesWithInlining) {

  std::stringstream ss(
      "com.foo.Bar -> X.A:\n"
      "    10:12:void caller():25:27 -> a\n"
      "    10:12:void inlined():30:31 -> a\n"
      "    10:12:void alsoInlined():42:43 -> a\n");

  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "LX/A;.a:()V" "SourceFile" 11)
      (const v1 0)
      (return-void)
    )
)");

  ProguardMap pm(ss);
  pg_impl::apply_deobfuscated_positions(code.get(), pm);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "Lcom/foo/Bar;.alsoInlined:()V" Bar.java 43)
      (.pos:dbg_1 "Lcom/foo/Bar;.inlined:()V" Bar.java 31 dbg_0)
      (.pos:dbg_2 "Lcom/foo/Bar;.caller:()V" Bar.java 26 dbg_1)
      (const v1 0)
      (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ProguardMapTest, DeobfuscateFramesWithoutLineRange) {

  std::stringstream ss(
      "com.foo.Bar -> X.A:\n"
      "    1:30:void qux() -> a\n"
      "    1:30:void flux():5 -> b\n");

  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "LX/A;.a:()V" "SourceFile" 24)
      (const v1 0)
      (.pos:dbg_1 "LX/A;.b:()V" "SourceFile" 24)
      (return-void)
    )
)");

  ProguardMap pm(ss);
  pg_impl::apply_deobfuscated_positions(code.get(), pm);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos:dbg_0 "Lcom/foo/Bar;.qux:()V" Bar.java 24)
      (const v1 0)
      (.pos:dbg_1 "Lcom/foo/Bar;.flux:()V" Bar.java 5)
      (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
