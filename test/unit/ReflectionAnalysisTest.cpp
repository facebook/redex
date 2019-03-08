/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "LocalDce.h"
#include "RedexContext.h"
#include "ReflectionAnalysis.h"

using namespace testing;
using namespace reflection;

class ReflectionAnalysisTest : public ::testing::Test {
 public:
  ~ReflectionAnalysisTest() { delete g_redex; }

  ReflectionAnalysisTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({
        get_object_type() // v5
    });
    auto proto = DexProto::make_proto(get_void_type(), args);
    m_method = static_cast<DexMethod*>(
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testMethod"),
                               proto));
    m_method->set_deobfuscated_name("testMethod");
    m_method->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    m_method->set_code(std::make_unique<IRCode>(m_method, /* temp_regs */ 5));
  }

  void add_code(const std::unique_ptr<IRCode>& insns) {
    IRCode* code = m_method->get_code();
    for (const auto& insn : *insns) {
      code->push_back(insn);
    }
  }

  std::string to_string(const ReflectionSites& reflection_sites) {
    std::ostringstream out;
    for (const auto it : reflection_sites) {
      out << SHOW(it.first) << " {";
      for (auto iit = it.second.begin(); iit != it.second.end(); ++iit) {
        out << iit->first << ", " << iit->second;
        if (std::next(iit) != it.second.end()) {
          out << ";";
        }
      }
      out << "}" << std::endl;
    }

    return out.str();
  }

 protected:
  DexMethod* m_method;
};

TEST_F(ReflectionAnalysisTest, noReflection) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-string "S1")
      (move-result-pseudo-object v1)
      (filled-new-array (v1) "[Ljava/lang/String;")
      (move-result-object v0)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_FALSE(analysis.has_found_reflection());
}

TEST_F(ReflectionAnalysisTest, constClass) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (const-class "LFoo;")
      (move-result-pseudo-object v1)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(to_string(analysis.get_reflection_sites()),
            "IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1 {4294967294, "
            "CLASS{LFoo;}(REFLECTION)}\n");
}

TEST_F(ReflectionAnalysisTest, getClassOnParam) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (invoke-virtual (v6) "Ljava/lang/Object;.getClass:()Ljava/lang/Class;")
      (move-result-object v1)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(to_string(analysis.get_reflection_sites()),
            "MOVE_RESULT_OBJECT v1 {4294967294, "
            "CLASS{Ljava/lang/Object;}(REFLECTION)}\n");
}

TEST_F(ReflectionAnalysisTest, classForName) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-string "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-static (v1) "Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class;")
      (move-result-object v0)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(to_string(analysis.get_reflection_sites()),
            "MOVE_RESULT_OBJECT v0 {4294967294, CLASS{LLFoo;;}(REFLECTION)}\n");
}

TEST_F(ReflectionAnalysisTest, getClassOnField) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (iget-object v5 "LFoo;.bar:Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (invoke-virtual (v1) "Ljava/lang/Object;.getClass:()Ljava/lang/Class;")
      (move-result-object v1)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(to_string(analysis.get_reflection_sites()),
            "MOVE_RESULT_OBJECT v1 {4294967294, "
            "CLASS{Ljava/lang/String;}(REFLECTION)}\n");
}

TEST_F(ReflectionAnalysisTest, getMethod) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-class "LFoo;")
      (move-result-pseudo-object v1)
      (const-string "bar")
      (move-result-pseudo-object v2)
      (new-array v3 "[Ljava/lang/Class;")
      (move-result-pseudo-object v3)
      (invoke-virtual (v1 v2 v3) "Ljava/lang/Class;.getMethod:(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;")
      (move-result-object v4)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(
      to_string(analysis.get_reflection_sites()),
      "IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1 {4294967294, CLASS{LFoo;}(REFLECTION)}\n\
CONST_STRING \"bar\" {1, CLASS{LFoo;}(REFLECTION);4294967294, CLASS{LFoo;}(REFLECTION)}\n\
IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v2 {1, CLASS{LFoo;}(REFLECTION)}\n\
NEW_ARRAY v3, [Ljava/lang/Class; {1, CLASS{LFoo;}(REFLECTION)}\n\
IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v3 {1, CLASS{LFoo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, v2, v3, Ljava/lang/Class;.getMethod:(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method; {1, CLASS{LFoo;}(REFLECTION)}\n\
MOVE_RESULT_OBJECT v4 {1, CLASS{LFoo;}(REFLECTION);4294967294, METHOD{LFoo;:bar}}\n");
}

TEST_F(ReflectionAnalysisTest, getField) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-class "LFoo;")
      (move-result-pseudo-object v1)
      (const-string "bar")
      (move-result-pseudo-object v2)
      (invoke-virtual (v1 v2) "Ljava/lang/Class;.getField:(Ljava/lang/String;)Ljava/lang/reflect/Field;")
      (move-result-object v4)
    )
  )");
  add_code(insns);
  ReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(
      to_string(analysis.get_reflection_sites()),
      "IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1 {4294967294, CLASS{LFoo;}(REFLECTION)}\n\
CONST_STRING \"bar\" {1, CLASS{LFoo;}(REFLECTION);4294967294, CLASS{LFoo;}(REFLECTION)}\n\
IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v2 {1, CLASS{LFoo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, v2, Ljava/lang/Class;.getField:(Ljava/lang/String;)Ljava/lang/reflect/Field; {1, CLASS{LFoo;}(REFLECTION)}\n\
MOVE_RESULT_OBJECT v4 {1, CLASS{LFoo;}(REFLECTION);4294967294, FIELD{LFoo;:bar}}\n");
}
