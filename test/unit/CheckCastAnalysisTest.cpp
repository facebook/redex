/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CheckCastAnalysis.h"
#include "IRAssembler.h"
#include "RedexTest.h"

struct CheckCastAnalysisTest : public RedexTest {};

TEST_F(CheckCastAnalysisTest, simple_string) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
      (
        (const-string "S1")
        (move-result-pseudo-object v1)
        (check-cast v1 "Ljava/lang/String;")
        (move-result-pseudo-object v1)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::impl::CheckCastAnalysis analysis(method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "Ljava/lang/String;");
  EXPECT_EQ(it->replacement, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, new_instance) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
      (
        (new-instance "LFoo;")
        (move-result-pseudo-object v1)
        (check-cast v1 "LFoo;")
        (move-result-pseudo-object v1)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::impl::CheckCastAnalysis analysis(method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LFoo;");
  EXPECT_EQ(it->replacement, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, parameter) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LBar;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::impl::CheckCastAnalysis analysis(method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LBar;");
  EXPECT_NE(it->replacement, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, this_parameter) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LBar;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v0 "LFoo;")
        (move-result-pseudo-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::impl::CheckCastAnalysis analysis(method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LFoo;");
  EXPECT_EQ(it->replacement, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, get_field) {

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
      (
        (iget-object v0 "LFoo;.b:LBar;")
        (move-result-pseudo-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v2)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::impl::CheckCastAnalysis analysis(method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LBar;");
  EXPECT_NE(it->replacement, boost::none);
  method->get_code()->clear_cfg();
}
