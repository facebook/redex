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
    (method (public) "LFoo;.bar:()Ljava/lang/String;"
      (
        (const-string "S1")
        (move-result-pseudo-object v1)
        (check-cast v1 "Ljava/lang/String;")
        (move-result-pseudo-object v1)
        (return-object v1)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "Ljava/lang/String;");
  EXPECT_EQ(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, new_instance) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()LFoo;"
      (
        (new-instance "LFoo;")
        (move-result-pseudo-object v1)
        (check-cast v1 "LFoo;")
        (move-result-pseudo-object v1)
        (return-object v1)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LFoo;");
  EXPECT_EQ(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, parameter) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LBar;)LBar;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LBar;");
  EXPECT_NE(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, array_parameter) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:([LBar;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "Ljava/lang/Object;")
        (move-result-pseudo-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "Ljava/lang/Object;");
  EXPECT_NE(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, this_parameter) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LBar;)LFoo;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v0 "LFoo;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LFoo;");
  EXPECT_EQ(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, get_field) {

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()LBar;"
      (
        (iget-object v0 "LFoo;.b:LBar;")
        (move-result-pseudo-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v2)
        (return-object v2)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config{.weaken = false};
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LBar;");
  EXPECT_NE(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);

  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, weaken_disabled) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config{.weaken = false};
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 0);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, weaken_replace) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LBar;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LBar;");
  EXPECT_NE(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, weaken) {
  auto a_type = DexType::make_type("LA;");
  auto b_type = DexType::make_type("LB;");
  auto c_type = DexType::make_type("LC;");
  ClassCreator a_creator(a_type);
  a_creator.set_super(type::java_lang_Object());
  ClassCreator b_creator(b_type);
  b_creator.set_super(a_type);
  ClassCreator c_creator(c_type);
  c_creator.set_super(b_type);
  a_creator.create();
  b_creator.create();
  c_creator.create();

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LA;)LB;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LC;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LC;");
  EXPECT_EQ(it->replacement_insn, boost::none);
  EXPECT_NE(it->replacement_type, boost::none);
  EXPECT_EQ(*it->replacement_type, b_type);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, weaken_interface_to_interface) {
  auto i_type = DexType::make_type("LI;");
  ClassCreator i_creator(i_type);
  i_creator.set_access(ACC_INTERFACE | ACC_ABSTRACT);
  i_creator.set_super(type::java_lang_Object());
  auto j_type = DexType::make_type("LJ;");
  ClassCreator j_creator(j_type);
  j_creator.set_access(ACC_INTERFACE | ACC_ABSTRACT);
  j_creator.set_super(type::java_lang_Object());
  j_creator.add_interface(i_type);
  auto k_type = DexType::make_type("LK;");
  ClassCreator k_creator(k_type);
  k_creator.set_access(ACC_INTERFACE | ACC_ABSTRACT);
  k_creator.set_super(type::java_lang_Object());
  k_creator.add_interface(i_type);
  i_creator.create();
  j_creator.create();
  k_creator.create();

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LI;)LJ;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LK;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LK;");
  EXPECT_EQ(it->replacement_insn, boost::none);
  EXPECT_NE(it->replacement_type, boost::none);
  EXPECT_EQ(*it->replacement_type, j_type);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, weaken_replace_class_to_interface) {
  auto i_type = DexType::make_type("LI;");
  ClassCreator i_creator(i_type);
  i_creator.set_access(ACC_INTERFACE | ACC_ABSTRACT);
  i_creator.set_super(type::java_lang_Object());

  auto a_type = DexType::make_type("LA;");
  auto b_type = DexType::make_type("LB;");
  ClassCreator a_creator(a_type);
  a_creator.set_super(type::java_lang_Object());
  a_creator.add_interface(i_type);
  ClassCreator b_creator(b_type);
  b_creator.set_super(a_type);
  b_creator.add_interface(i_type);
  i_creator.create();
  a_creator.create();
  b_creator.create();

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LA;)LI;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LB;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 1);
  auto it = replacements.begin();
  auto insn = it->insn;
  EXPECT_EQ(insn->opcode(), OPCODE_CHECK_CAST);
  EXPECT_EQ(insn->get_type()->get_name()->str(), "LB;");
  EXPECT_NE(it->replacement_insn, boost::none);
  EXPECT_EQ(it->replacement_type, boost::none);
  method->get_code()->clear_cfg();
}

TEST_F(CheckCastAnalysisTest, do_not_weaken_class_to_interface) {
  auto i_type = DexType::make_type("LI;");
  ClassCreator i_creator(i_type);
  i_creator.set_access(ACC_INTERFACE | ACC_ABSTRACT);
  i_creator.set_super(type::java_lang_Object());

  auto a_type = DexType::make_type("LA;");
  auto b_type = DexType::make_type("LB;");
  ClassCreator a_creator(a_type);
  a_creator.set_super(type::java_lang_Object());
  ClassCreator b_creator(b_type);
  b_creator.set_super(a_type);
  b_creator.add_interface(i_type);
  i_creator.create();
  a_creator.create();
  b_creator.create();

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(LA;)LI;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (check-cast v1 "LB;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  method->get_code()->build_cfg(true);
  check_casts::CheckCastConfig config;
  check_casts::impl::CheckCastAnalysis analysis(config, method);
  auto replacements = analysis.collect_redundant_checks_replacement();

  EXPECT_EQ(replacements.size(), 0);
}
