/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional/optional_io.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"
#include "SwitchEquivFinder.h"

using namespace testing;

namespace {
void setup() {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());
  auto field = DexField::make_field("LFoo;.table:[LBar;")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC);
  cc.add_field(field);
  cc.create();
}

cfg::InstructionIterator get_first_branch(cfg::ControlFlowGraph& cfg) {
  auto iterable = InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (opcode::is_branch(it->insn->opcode())) {
      return it;
    }
  }
  return iterable.end();
}

cfg::InstructionIterator get_first_occurrence(cfg::ControlFlowGraph& cfg,
                                              const IROpcode& opcode) {
  auto iterable = InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == opcode) {
      return it;
    }
  }
  return iterable.end();
}
} // namespace

class SwitchEquivFinderTest : public RedexTest {};

TEST_F(SwitchEquivFinderTest, if_chain) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 0);
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

TEST_F(SwitchEquivFinderTest, extra_loads_intersect) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 0);
  ASSERT_FALSE(finder.success());
}

TEST_F(SwitchEquivFinderTest, extra_loads_wide) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 0);
  ASSERT_FALSE(finder.success());
}

TEST_F(SwitchEquivFinderTest, extra_loads_wide2) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 0);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, overwrite) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 1);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, overwrite_wide) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 1);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, loop) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 1);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, other_entry_points) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_occurrence(cfg, OPCODE_IF_LE), 1);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, other_entry_points2) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_occurrence(cfg, OPCODE_IF_LE), 1);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, goto_default) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 1);
  ASSERT_TRUE(finder.success());
  code->clear_cfg();
}

TEST_F(SwitchEquivFinderTest, divergent_leaf_entry_state) {
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
  auto& cfg = code->cfg();
  SwitchEquivFinder finder(&cfg, get_first_branch(cfg), 0);
  ASSERT_FALSE(finder.success());
  code->clear_cfg();
}
