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
#include "SimpleReflectionAnalysis.h"

using namespace testing;
using namespace sra;

class SimpleReflectionAnalysisTest : public ::testing::Test {
 public:
  ~SimpleReflectionAnalysisTest() { delete g_redex; }

  SimpleReflectionAnalysisTest() {
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

 protected:
  DexMethod* m_method;
};

TEST_F(SimpleReflectionAnalysisTest, noReflection) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-string "S1")
      (move-result-pseudo-object v1)
      (filled-new-array (v1) "[Ljava/lang/String;")
      (move-result-object v0)
      (return-void)
    )
  )");
  add_code(insns);
  SimpleReflectionAnalysis analysis(m_method);
  EXPECT_FALSE(analysis.has_found_reflection());
}

TEST_F(SimpleReflectionAnalysisTest, constClass) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (const-class "LFoo;")
      (move-result-pseudo-object v1)
      (return-void)
    )
  )");
  add_code(insns);
  SimpleReflectionAnalysis analysis(m_method);
  EXPECT_TRUE(analysis.has_found_reflection());
}
