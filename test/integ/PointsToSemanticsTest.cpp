/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PointsToSemantics.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "DexUtil.h"
#include "IRAssembler.h"
#include "JarLoader.h"
#include "RedexTest.h"

using namespace sparta;

std::set<std::string> method_semantics = {
    // PointsToSemantics' class initializer
    "Lcom/facebook/redextest/PointsToSemantics;#<clinit>: ()V {\n"
    " V0 = NEW Lcom/facebook/redextest/PointsToSemantics$A;\n"
    " V0.{D}Lcom/facebook/redextest/PointsToSemantics$A;#<init>()\n"
    " Lcom/facebook/redextest/PointsToSemantics;#a1 = V0\n"
    " V1 = NEW Lcom/facebook/redextest/PointsToSemantics$A;\n"
    " V2 = \"something\"\n"
    " V1.{D}Lcom/facebook/redextest/PointsToSemantics$A;#<init>(1 => V2)\n"
    " Lcom/facebook/redextest/PointsToSemantics;#a2 = V1\n"
    "}\n",
    // PointsToSemantics' constructor
    "Lcom/facebook/redextest/PointsToSemantics;#<init>: ()V {\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    "}\n",
    // I#f()
    "Lcom/facebook/redextest/PointsToSemantics$I;#f: "
    "()Lcom/facebook/redextest/PointsToSemantics$I; = ABSTRACT\n",
    // Base's constructor
    "Lcom/facebook/redextest/PointsToSemantics$Base;#<init>: "
    "(Lcom/facebook/redextest/PointsToSemantics;)V {\n"
    " V1 = PARAM 0\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$Base;#this$0 = V1\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    "}\n",
    // Base#f()
    "Lcom/facebook/redextest/PointsToSemantics$Base;#f: "
    "()Lcom/facebook/redextest/PointsToSemantics$I; {\n"
    " V1 = NEW Lcom/facebook/redextest/PointsToSemantics$Base;\n"
    " V2 = THIS.Lcom/facebook/redextest/PointsToSemantics$Base;#this$0\n"
    " V1.{D}Lcom/facebook/redextest/PointsToSemantics$Base;#<init>(0 => V2)\n"
    " RETURN V1\n"
    "}\n",
    // X's constructor
    "Lcom/facebook/redextest/PointsToSemantics$X;#<init>: "
    "(Lcom/facebook/redextest/PointsToSemantics;)V {\n"
    " V1 = PARAM 0\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$X;#this$0 = V1\n"
    " THIS.{D}Lcom/facebook/redextest/PointsToSemantics$Base;#<init>(0 => V1)\n"
    "}\n",
    // X#g()
    "Lcom/facebook/redextest/PointsToSemantics$X;#g: "
    "()Lcom/facebook/redextest/PointsToSemantics$I; {\n"
    " V1 = THIS.{S}Lcom/facebook/redextest/PointsToSemantics$Base;#f()\n"
    " RETURN V1\n"
    "}\n",
    // PointsToSemantics#cast()
    "Lcom/facebook/redextest/PointsToSemantics;#cast: "
    "(Lcom/facebook/redextest/PointsToSemantics$I;)Lcom/facebook/redextest/"
    "PointsToSemantics$I; {\n"
    " V0 = PARAM 0\n"
    " V1 = CAST<Lcom/facebook/redextest/PointsToSemantics$X;>(V0)\n"
    " V2 = V1.{V}Lcom/facebook/redextest/PointsToSemantics$X;#g()\n"
    " V4 = V2 U V3\n"
    " RETURN V4\n"
    " V3 = V0.{I}Lcom/facebook/redextest/PointsToSemantics$I;#f()\n"
    "}\n",
    // A's constructor 1
    "Lcom/facebook/redextest/PointsToSemantics$A;#<init>: "
    "(ILjava/lang/String;)V {\n"
    " V1 = PARAM 1\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    " V2 = NEW Ljava/util/ArrayList;\n"
    " V2.{D}Ljava/util/ArrayList;#<init>()\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$A;#m_list = V2\n"
    " V3 = THIS.Lcom/facebook/redextest/PointsToSemantics$A;#m_list\n"
    " V3.{V}Ljava/util/ArrayList;#add(0 => V1)\n"
    "}\n",
    // A's constructor 2
    "Lcom/facebook/redextest/PointsToSemantics$A;#<init>: (I)V {\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    " V1 = NEW Ljava/util/ArrayList;\n"
    " V1.{D}Ljava/util/ArrayList;#<init>()\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$A;#m_list = V1\n"
    "}\n",
    // B's constructor
    "Lcom/facebook/redextest/PointsToSemantics$B;#<init>: ()V {\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    "}\n",
    // B#strs()
    "Lcom/facebook/redextest/PointsToSemantics$B;#strs: ()[Ljava/lang/String;"
    " {\n"
    " V0 = NEW [Ljava/lang/String;\n"
    " V1 = \"a\"\n"
    " ARRAY_ELEM(V0) = V1\n"
    " V2 = \"b\"\n"
    " ARRAY_ELEM(V0) = V2\n"
    " V3 = \"c\"\n"
    " ARRAY_ELEM(V0) = V3\n"
    " V4 = \"d\"\n"
    " ARRAY_ELEM(V0) = V4\n"
    " RETURN V0\n"
    "}\n",
    // B#ints()
    "Lcom/facebook/redextest/PointsToSemantics$B;#ints: ()[I {\n"
    " V0 = NEW [I\n"
    " RETURN V0\n"
    "}\n",
    // B#pick()
    "Lcom/facebook/redextest/PointsToSemantics$B;#pick: (I)Ljava/lang/String;"
    " {\n"
    " V1 = Lcom/facebook/redextest/PointsToSemantics$B;#strs()\n"
    " Lcom/facebook/redextest/PointsToSemantics$B;#ints()\n"
    " V3 = ARRAY_ELEM(V1)\n"
    " RETURN V3\n"
    "}\n",
    // Processor#run()
    "Lcom/facebook/redextest/PointsToSemantics$Processor;#run: ()V = "
    "ABSTRACT\n",
    // Time's constructor
    "Lcom/facebook/redextest/PointsToSemantics$Time;#<init>: (J)V {\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    "}\n",
    // Time#sleep
    "Lcom/facebook/redextest/PointsToSemantics$Time;#sleep: (J)V {\n"
    "}\n",
    // Time#repeat()
    "Lcom/facebook/redextest/PointsToSemantics$Time;#repeat: "
    "(JLcom/facebook/redextest/PointsToSemantics$Processor;)V {\n"
    " V1 = PARAM 1\n"
    " V1.{I}Lcom/facebook/redextest/PointsToSemantics$Processor;#run()\n"
    " Ljava/lang/Math;#max()\n"
    " THIS.{V}Lcom/facebook/redextest/PointsToSemantics$Time;#sleep()\n"
    "}\n",
    // C's constructor
    "Lcom/facebook/redextest/PointsToSemantics$C;#<init>: "
    "(Lcom/facebook/redextest/PointsToSemantics$A;Lcom/facebook/redextest/"
    "PointsToSemantics$C;)V {\n"
    " V1 = PARAM 0\n"
    " V2 = PARAM 1\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$C;#next = V2\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$C;#val = V1\n"
    "}\n",
    // C#nth()
    "Lcom/facebook/redextest/PointsToSemantics$C;#nth: "
    "(I)Lcom/facebook/redextest/PointsToSemantics$A; {\n"
    " V3 = THIS U V1\n"
    " V1 = V3.Lcom/facebook/redextest/PointsToSemantics$C;#next\n"
    " V2 = V3.Lcom/facebook/redextest/PointsToSemantics$C;#val\n"
    " RETURN V2\n"
    "}\n",
    // PointsToSemantics#extract()
    "Lcom/facebook/redextest/PointsToSemantics;#extract: "
    "()Lcom/facebook/redextest/PointsToSemantics$A; {\n"
    " V1 = NEW Lcom/facebook/redextest/PointsToSemantics$C;\n"
    " V2 = Lcom/facebook/redextest/PointsToSemantics;#a1\n"
    " V3 = NEW Lcom/facebook/redextest/PointsToSemantics$C;\n"
    " V4 = Lcom/facebook/redextest/PointsToSemantics;#a2\n"
    " V3.{D}Lcom/facebook/redextest/PointsToSemantics$C;#<init>(0 => V4, 1 => "
    "NULL)\n"
    " V1.{D}Lcom/facebook/redextest/PointsToSemantics$C;#<init>(0 => V2, 1 => "
    "V3)\n"
    " V5 = V1.{V}Lcom/facebook/redextest/PointsToSemantics$C;#nth()\n"
    " RETURN V5\n"
    "}\n",
    // PointsToSemantics#nativeMethod()
    "Lcom/facebook/redextest/PointsToSemantics;#nativeMethod: ()[I = NATIVE\n",
    // AnException's constructor
    "Lcom/facebook/redextest/PointsToSemantics$AnException;#<init>: ()V {\n"
    " THIS.{D}Ljava/lang/Exception;#<init>()\n"
    "}\n",
    // PointsToSemantics#arrayOfX()
    "Lcom/facebook/redextest/PointsToSemantics;#arrayOfX: "
    "(I)[Lcom/facebook/redextest/PointsToSemantics$X; {\n"
    " V1 = EXCEPTION\n"
    " V1.{D}Lcom/facebook/redextest/PointsToSemantics$AnException;#<init>()\n"
    " V2 = NEW [Lcom/facebook/redextest/PointsToSemantics$X;\n"
    " RETURN V2\n"
    "}\n",
    // PointsToSemantics#runOnArrayOfX()
    "Lcom/facebook/redextest/PointsToSemantics;#runOnArrayOfX: "
    "(I)Lcom/facebook/redextest/PointsToSemantics$I; {\n"
    " V1 = THIS.{V}Lcom/facebook/redextest/PointsToSemantics;#arrayOfX()\n"
    " V2 = ARRAY_ELEM(V1)\n"
    " V3 = Lcom/facebook/redextest/PointsToSemantics;#cast(0 => V2)\n"
    " ARRAY_ELEM(V1) = V3\n"
    " V4 = ARRAY_ELEM(V1)\n"
    " V9 = V4 U V8\n"
    " RETURN V9\n"
    " V5 = EXCEPTION\n"
    " V6 = Ljava/lang/System;#out\n"
    " V7 = V5.{V}Lcom/facebook/redextest/"
    "PointsToSemantics$AnException;#getMessage()\n"
    " V6.{V}Ljava/io/PrintStream;#println(0 => V7)\n"
    " V8 = NEW Lcom/facebook/redextest/PointsToSemantics$Base;\n"
    " V8.{D}Lcom/facebook/redextest/PointsToSemantics$Base;#<init>(0 => THIS)\n"
    "}\n",
    // PointsToSemantics#longMethod()
    "Lcom/facebook/redextest/PointsToSemantics;#longMethod: (JJJJJI[J)[J {\n"
    " V1 = PARAM 6\n"
    " RETURN V1\n"
    "}\n",
    // Complex's constructor
    "Lcom/facebook/redextest/PointsToSemantics$Complex;#<init>: "
    "(Lcom/facebook/redextest/PointsToSemantics;)V {\n"
    " V1 = PARAM 0\n"
    " THIS.Lcom/facebook/redextest/PointsToSemantics$Complex;#this$0 = V1\n"
    " THIS.{D}Ljava/lang/Object;#<init>()\n"
    "}\n",
    // PointsToSemantics#unusedFields()
    "Lcom/facebook/redextest/PointsToSemantics;#unusedFields: "
    "(Lcom/facebook/redextest/PointsToSemantics$Complex;)I {\n"
    " V1 = PARAM 0\n"
    " V2 = V1.Lcom/facebook/redextest/PointsToSemantics$Complex;#c\n"
    " V3 = V2.Lcom/facebook/redextest/PointsToSemantics$Complex;#c\n"
    " V4 = V3.Lcom/facebook/redextest/PointsToSemantics$Complex;#b\n"
    " V4.{V}Lcom/facebook/redextest/PointsToSemantics$B;#pick()\n"
    "}\n",
    // PointsToSemantics#getClassTest()
    "Lcom/facebook/redextest/PointsToSemantics;#getClassTest: "
    "()[Ljava/lang/Class; {\n"
    " V1 = NEW [Ljava/lang/Class;\n"
    " V2 = \"foo\"\n"
    " V3 = GET_CLASS(V2)\n"
    " ARRAY_ELEM(V1) = V3\n"
    " V4 = THIS.{V}Lcom/facebook/redextest/PointsToSemantics;#nativeMethod()\n"
    " V5 = GET_CLASS(V4)\n"
    " ARRAY_ELEM(V1) = V5\n"
    " V6 = NEW Ljava/util/HashSet;\n"
    " V6.{D}Ljava/util/HashSet;#<init>()\n"
    " V7 = GET_CLASS(V6)\n"
    " ARRAY_ELEM(V1) = V7\n"
    " V8 = CLASS<Ljava/lang/Boolean;>\n"
    " ARRAY_ELEM(V1) = V8\n"
    " V9 = CLASS<Ljava/lang/Integer;>\n"
    " ARRAY_ELEM(V1) = V9\n"
    " V10 = CLASS<Ljava/lang/Short;>\n"
    " ARRAY_ELEM(V1) = V10\n"
    " V11 = CLASS<Ljava/lang/Long;>\n"
    " ARRAY_ELEM(V1) = V11\n"
    " V12 = CLASS<Ljava/lang/Float;>\n"
    " ARRAY_ELEM(V1) = V12\n"
    " V13 = CLASS<Ljava/lang/Double;>\n"
    " ARRAY_ELEM(V1) = V13\n"
    " V14 = CLASS<Ljava/lang/Void;>\n"
    " ARRAY_ELEM(V1) = V14\n"
    " V15 = CLASS<Ljava/lang/Character;>\n"
    " ARRAY_ELEM(V1) = V15\n"
    " V16 = CLASS<Ljava/io/PrintStream;>\n"
    " ARRAY_ELEM(V1) = V16\n"
    " V17 = CLASS<[[[Lcom/facebook/redextest/PointsToSemantics$A;>\n"
    " ARRAY_ELEM(V1) = V17\n"
    " RETURN V1\n"
    "}\n",
    "Lcom/facebook/redextest/PointsToSemantics;#filledNewArrayTest: "
    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/"
    "String; {\n"
    " V0 = PARAM 0\n"
    " V1 = PARAM 1\n"
    " V2 = PARAM 2\n"
    " V3 = NEW [Ljava/lang/String;\n"
    " ARRAY_ELEM(V3) = V0\n"
    " ARRAY_ELEM(V3) = V1\n"
    " ARRAY_ELEM(V3) = V2\n"
    " RETURN V3\n"
    "}\n",
};

void patch_filled_new_array_test(Scope& scope) {
  for (DexClass* dex_class : scope) {
    for (DexMethod* dmethod : dex_class->get_dmethods()) {
      if (dmethod->get_name()->str() == "filledNewArrayTest") {
        auto c = assembler::ircode_from_string(R"(
          (
            (load-param-object v0)
            (load-param-object v1)
            (load-param-object v2)
            (filled-new-array (v0 v1 v2) "[Ljava/lang/String;")
            (move-result-object v0)
            (return-object v0)
          )
        )");
        dmethod->set_code(std::move(c));
        return;
      }
    }
  }
}

class PointsToSemanticsTest : public RedexIntegrationTest {};

TEST_F(PointsToSemanticsTest, semanticActionGeneration) {
  const char* android_env_sdk = std::getenv("ANDROID_SDK");
  const char* android_config_sdk = std::getenv("sdk_path");

  const char* android_sdk = (strncmp(android_config_sdk, "None", 4) != 0)
                                ? android_config_sdk
                                : android_env_sdk;

  ASSERT_NE(nullptr, android_sdk);
  const char* android_target = std::getenv("android_target");
  ASSERT_NE(nullptr, android_target);
  std::string android_version(android_target);
  ASSERT_NE("NotFound", android_version);
  std::string sdk_jar = std::string(android_sdk) + "/platforms/" +
                        android_version + "/android.jar";
  ASSERT_TRUE(load_jar_file(DexLocation::make_location("", sdk_jar)));

  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);

  // The generation of filled-new-array instructions depends on the Dex compiler
  // used and seems hard to predict. We patch the method body in order to get
  // the code we want.
  patch_filled_new_array_test(scope);

  PointsToSemantics pt_semantics(scope);

  std::set<std::string> pt_output;
  for (const auto& pt_entry : pt_semantics) {
    std::ostringstream out;
    EXPECT_NE(PTS_STUB, pt_entry.second.kind());
    out << pt_entry.second;
    pt_output.insert(out.str());
  }
  EXPECT_THAT(pt_output, ::testing::ContainerEq(method_semantics));

  // Testing the serialization mechanism based on S-expressions.
  std::stringstream serialization;
  for (const auto& pt_entry : pt_semantics) {
    auto e = pt_entry.second.to_s_expr();
    serialization << e;
  }
  s_expr_istream input(serialization);
  std::set<std::string> deserialization;
  while (input.good()) {
    s_expr e;
    input >> e;
    if (input.eoi()) {
      break;
    }
    ASSERT_TRUE(!input.fail()) << input.what();
    auto semantics_opt = PointsToMethodSemantics::from_s_expr(e);
    ASSERT_TRUE(semantics_opt) << e;
    std::ostringstream out;
    out << *semantics_opt;
    deserialization.insert(out.str());
  }
  EXPECT_THAT(deserialization, ::testing::ContainerEq(method_semantics));
}
