/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "EnumInSwitch.h"
#include "IRAssembler.h"
#include "SwitchEquivFinder.h"

void setup() {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(get_object_type());
  auto field =
      static_cast<DexField*>(DexField::make_field("LFoo;.table:[LBar;"));
  field->make_concrete(
      ACC_PUBLIC | ACC_STATIC,
      DexEncodedValue::zero_for_type(get_array_type(get_object_type())));
  cc.add_field(field);
  cc.create();
}

std::vector<optimize_enums::Info> find_enums(cfg::ControlFlowGraph* cfg) {
  cfg->calculate_exit_block();
  optimize_enums::Iterator fixpoint(cfg);
  fixpoint.run(optimize_enums::Environment());
  return fixpoint.collect();
}

TEST(OptimizeEnums, basic_neg) {
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

TEST(OptimizeEnums, basic_pos) {
  g_redex = new RedexContext();
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
      (sparse-switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(1, find_enums(&code->cfg()).size());
  code->clear_cfg();
  delete g_redex;
}

TEST(OptimizeEnums, overwritten) {
  g_redex = new RedexContext();
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
      (sparse-switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(0, find_enums(&code->cfg()).size());
  code->clear_cfg();
  delete g_redex;
}

TEST(OptimizeEnums, nested) {
  g_redex = new RedexContext();
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
      (packed-switch v0 (:a))

      (return-void)

      (:a 1)
      (const v1 0)
      (invoke-virtual (v1) "Ljava/lang/Integer;.intValue:()I")
      (goto :x)

      (:x)
      (move-result v0)
      (packed-switch v0 (:b))

      (return-void)

      (:b 1)
      (return-void)
    )
)");

  code->build_cfg();
  EXPECT_EQ(1, find_enums(&code->cfg()).size());
  code->clear_cfg();
  delete g_redex;
}

TEST(OptimizeEnums, if_chain) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, extra_loads_intersect) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, extra_loads_wide) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, extra_loads_wide2) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, overwrite) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, overwrite_wide) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, loop) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, other_entry_points) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, other_entry_points2) {
  g_redex = new RedexContext();
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
  delete g_redex;
}

TEST(OptimizeEnums, goto_default) {
  g_redex = new RedexContext();
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
      (sparse-switch v1 (:a :b))

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
  delete g_redex;
}

TEST(OptimizeEnums, divergent_leaf_entry_state) {
  g_redex = new RedexContext();
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
  delete g_redex;
}
