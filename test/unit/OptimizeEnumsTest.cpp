/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional/optional_io.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "EnumConfig.h"
#include "EnumInSwitch.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "SwitchEquivFinder.h"

using namespace testing;

void setup() {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());
  auto field = DexField::make_field("LFoo;.table:[LBar;")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC);
  cc.add_field(field);
  cc.create();
}

std::vector<optimize_enums::Info> find_enums(cfg::ControlFlowGraph* cfg) {
  cfg->calculate_exit_block();
  optimize_enums::Iterator fixpoint(cfg);
  fixpoint.run(optimize_enums::Environment());
  return fixpoint.collect();
}

class OptimizeEnumsTest : public RedexTest {};

TEST_F(OptimizeEnumsTest, basic_neg) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(0, find_enums(&code->cfg()).size());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, basic_pos) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(1, find_enums(&code->cfg()).size());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, overwritten) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v0 0)
      (switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(0, find_enums(&code->cfg()).size());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, nested) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (switch v0 (:a))

      (return-void)

      (:a 1)
      (const v1 0)
      (invoke-virtual (v1) "Ljava/lang/Integer;.intValue:()I")
      (goto :x)

      (:x)
      (move-result v0)
      (switch v0 (:b))

      (return-void)

      (:b 1)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(1, find_enums(&code->cfg()).size());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, if_chain) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v2 0)
      (if-eq v2 v0 :case0)

      (const v2 1)
      (if-eq v2 v0 :case1)

      (return v0)

      (:case0)
      (return v0)

      (:case1)
      (invoke-static (v2) "LFoo;.useReg:(I)V")
      (return v1)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_TRUE(finder.success());
  bool checked_one = false;
  bool checked_zero = false;
  bool found_fallthrough = false;
  for (const auto& key_and_case : finder.key_to_case()) {
    boost::optional<int32_t> key = key_and_case.first;
    cfg::Block* leaf = key_and_case.second;
    if (key == boost::none) {
      always_assert(!found_fallthrough);
      found_fallthrough = true;
      continue;
    }
    const auto& extra_loads = finder.extra_loads();
    const auto& search = extra_loads.find(leaf);
    if (key == 1) {
      EXPECT_NE(extra_loads.end(), search);
      const auto& loads = search->second;
      EXPECT_EQ(1, loads.size());
      auto reg_and_insn = *loads.begin();
      EXPECT_EQ(2, reg_and_insn.first);
      EXPECT_EQ(OPCODE_CONST, reg_and_insn.second->opcode());
      EXPECT_EQ(1, reg_and_insn.second->get_literal());
      checked_one = true;
    } else if (key == 0) {
      EXPECT_EQ(extra_loads.end(), search);
      checked_zero = true;
    }
  }
  EXPECT_TRUE(found_fallthrough);
  EXPECT_TRUE(checked_one);
  EXPECT_TRUE(checked_zero);
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, extra_loads_intersect) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (const v2 0)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v1 1)
      (if-gt v0 v1 :greater_than_one)

      (const v1 1)
      (if-ne v0 v1 :not_one)

      (:fallthrough)
      (return-void)

      (:greater_than_one)
      (const v2 1)
      (if-eqz v0 :case0)
      (goto :fallthrough)

      (:not_one)
      (if-eqz v0 :case0)
      (goto :fallthrough)

      (:case0)
      (invoke-static (v2) "LFoo;.useReg:(I)V")
      (return v0)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
}

TEST_F(OptimizeEnumsTest, extra_loads_wide) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (const v3 0)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v1 1)
      (if-gt v0 v1 :greater_than_one)

      (const v1 1)
      (if-ne v0 v1 :not_one)

      (:fallthrough)
      (return-void)

      (:greater_than_one)
      (const v3 1)
      (const-wide v2 1)
      (if-eqz v0 :case0)
      (goto :fallthrough)

      (:not_one)
      (const v3 1)
      (if-eqz v0 :case0)
      (goto :fallthrough)

      (:case0)
      (invoke-static (v3) "LFoo;.useReg:(I)V")
      (return v0)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
}

TEST_F(OptimizeEnumsTest, extra_loads_wide2) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v1 1)
      (if-lt v0 v1 :less_than_one)

      (:fallthrough)
      (return-void)

      (:less_than_one)
      (const-wide v2 1)
      (const v2 1)
      (if-eqz v0 :case0)
      (goto :fallthrough)

      (:case0)
      (invoke-static (v2) "LFoo;.useReg:(I)V")
      (return v0)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, overwrite) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (const v2 0)
      (if-le v2 v1 :case0)

      ; overwrite the switching reg, making this block a leaf
      (const v1 1)
      (if-eq v2 v1 :case1)

      (:case0)
      (return v0)

      (:case1)
      (return v1)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, overwrite_wide) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (const v2 0)
      (if-le v2 v1 :case0)

      ; overwrite the switching reg with the upper half of the load, making this
      ; block a leaf
      (const-wide v0 1)
      (if-eq v2 v1 :case1)

      (:case0)
      (return v0)

      (:case1)
      (return v1)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, loop) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (const v2 0)
      (if-le v2 v1 :case0)

      (:loop)
      (const v2 1)
      (if-eq v2 v1 :loop)

      (:case0)
      (return v0)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, other_entry_points) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (if-eqz v2 :case1)

      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (const v2 0)
      (if-le v2 v1 :case0)

      (const v2 1)
      (if-eq v2 v1 :case1)

      (:case0)
      (return v0)

      (:case1)
      (invoke-static (v2) "LFoo;.useReg:(I)V")
      (return v1)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, other_entry_points2) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (if-eqz v2 :non_leaf)

      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (const v2 0)
      (if-le v2 v1 :case0)

      (:non_leaf)
      (const v2 1)
      (if-eq v2 v1 :case1)

      (:case0)
      (return v0)

      (:case1)
      (invoke-static (v2) "LFoo;.useReg:(I)V")
      (return v1)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, goto_default) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v1)
      (switch v1 (:a :b))

      (:fallthrough)
      (return-void)

      (:a 0)
      (invoke-static (v1) "LFoo;.useReg:(I)V")
      (goto :fallthrough)

      (:b 1)
      (invoke-static (v1) "LFoo;.useReg:(I)V")
      (goto :fallthrough)
    )
)");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_TRUE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, divergent_leaf_entry_state) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v2 0)
      (invoke-virtual (v2) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v1 1)
      (if-eq v0 v1 :end)

      (const v1 2)
      (if-eq v0 v1 :end)

      (const v0 3)
      (return v0)

      (:end)
      (return v1)
    )
  )");

  code->build_cfg();
  const auto& results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  const auto& info = results[0];
  SwitchEquivFinder finder(&info.branch->cfg(), *info.branch, *info.reg);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, with_null_handling) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)

      (if-nez v1 :non-null-label)
      (const v0 -1)
      (goto :switch-label)

      (:non-null-label)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)

      (:switch-label)
      (switch v0 (:case_null :case_0 :case_1))

      ; Null handling
      (:case_null -1)
      (const v2 -1)
      (return v2)

      (:case_0 0)
      (const v2 0)
      (return v2)

      (:case_1 1)
      (const v2 1)
      (return v2)
    )
)");

  code->build_cfg();
  auto results = find_enums(&code->cfg());
  ASSERT_EQ(0, results.size());
  code->clear_cfg();
}

TEST_F(OptimizeEnumsTest, with_dead_null_handling) {
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)

      ; (if-nez v1 :non-null-label)
      ; (const v0 -1)
      ; (goto :switch-label)

      (:non-null-label)
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)

      (:switch-label)
      (switch v0 (:case_null :case_0 :case_1))

      ; Null handling
      (:case_null -1)
      (const v2 -1)
      (return v2)

      (:case_0 0)
      (const v2 0)
      (return v2)

      (:case_1 1)
      (const v2 1)
      (return v2)
    )
)");

  code->build_cfg();
  auto results = find_enums(&code->cfg());
  EXPECT_EQ(1, results.size());
  code->clear_cfg();
}

optimize_enums::ParamSummary get_summary(const std::string& s_expr) {
  auto method = assembler::method_from_string(s_expr);
  method->get_code()->build_cfg();
  return optimize_enums::calculate_param_summary(method,
                                                 type::java_lang_Object());
}

TEST_F(OptimizeEnumsTest, test_param_summary_generating) {
  auto summary = get_summary(R"(
    (method (static) "LFoo;.upcast_when_return:(Ljava/lang/Enum;)Ljava/lang/Object;"
      (
        (load-param-object v0)
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary.returned_param, boost::none);
  EXPECT_TRUE(summary.safe_params.empty());

  auto summary2 = get_summary(R"(
    (method (public) "LFoo;.param_0_is_not_safecast:(Ljava/lang/Enum;Ljava/lang/Object;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (load-param-object v2)
        (return-void)
      )
    )
  )");
  EXPECT_EQ(summary2.returned_param, boost::none);
  EXPECT_THAT(summary2.safe_params, UnorderedElementsAre(2));

  auto summary2_static = get_summary(R"(
    (method (static public) "LFoo;.param_0_is_not_safecast:(Ljava/lang/Enum;Ljava/lang/Object;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (return-void)
      )
    )
  )");
  EXPECT_EQ(summary2_static.returned_param, boost::none);
  EXPECT_THAT(summary2_static.safe_params, UnorderedElementsAre(1));

  auto summary3 = get_summary(R"(
    (method () "LFoo;.check_cast:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v1)
        (load-param-object v0)
        (check-cast v0 "Ljava/lang/Enum;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary3.returned_param, boost::none);
  EXPECT_TRUE(summary3.safe_params.empty());

  auto summary4 = get_summary(R"(
    (method () "LFoo;.has_invocation:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v1)
        (load-param-object v0)
        (invoke-virtual (v0) "Ljava/lang/Object;.toString:()Ljava/lang/String;")
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary4.returned_param, boost::none);
  EXPECT_TRUE(summary4.safe_params.empty());
}
