/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReflectionAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <unordered_map>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "RedexTest.h"

using namespace reflection;

std::unordered_map<std::string, std::string> expected = {
    {"f1", "\"foo1\""},
    {"f2", "\"foo2\""},
    {"m1", "\"moo1\""},
    {"m2", "\"moo2\""},
    {"f3", "OBJECT{Ljava/lang/String;}"},
    {"f4", "\"foo2\""},
    {"f5", "\"foo2\""},
    {"f6", "OBJECT{Ljava/lang/String;}"},
    {"m7", "OBJECT{Ljava/lang/String;}"},
    {"f8", ""}, // f8 is a generic string (name of field join("foo1", "foo2"))
    {"f9", "\"foo1\""}};

void validate_arguments(IRInstruction* insn,
                        const ReflectionAnalysis& analysis) {
  auto label = analysis.get_abstract_object(insn->src(0), insn);
  auto actual = analysis.get_abstract_object(insn->src(1), insn);
  EXPECT_TRUE(label);
  EXPECT_EQ(STRING, label->obj_kind);
  std::string label_str = label->dex_string->str_copy();
  std::string actual_str = "?";
  if (actual) {
    std::ostringstream out;
    out << *actual;
    actual_str = out.str();
  }
  auto it = expected.find(label_str);
  EXPECT_TRUE(it != expected.end());
  EXPECT_EQ(it->second, actual_str);
}

class SimpleReflectionAnalysisTest : public RedexIntegrationTest {};

TEST_F(SimpleReflectionAnalysisTest, nominalCases) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  for (const auto& cls : scope) {
    if (cls->get_name()->str() ==
        "Lcom/facebook/redextest/ReflectionAnalysis$Isolate;") {
      for (const auto& method : cls->get_dmethods()) {
        if (method->get_name()->str() == "main") {
          method->get_code()->build_cfg();
          auto& cfg = method->get_code()->cfg();
          ReflectionAnalysis analysis(method);
          for (auto& mie : InstructionIterable(cfg)) {
            IRInstruction* insn = mie.insn;
            if (insn->opcode() == OPCODE_INVOKE_STATIC &&
                insn->get_method()->get_name()->str() == "check") {
              validate_arguments(insn, analysis);
            }
          }
        }
      }
    }
  }
}
