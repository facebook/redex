/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UsedVarsAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DeadCodeEliminationPass.h"
#include "IRAssembler.h"
#include "RedexTest.h"

namespace ptrs = local_pointers;

using namespace testing;

class UsedVarsTest : public RedexTest {};

std::unique_ptr<UsedVarsFixpointIterator> analyze(IRCode* code) {
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  std::unordered_set<const DexMethod*> non_overridden_virtuals;
  side_effects::SummaryMap effect_summaries;
  ptrs::FixpointIterator pointers_fp_iter(cfg);
  pointers_fp_iter.run(ptrs::Environment());
  auto used_vars_fp_iter = std::make_unique<UsedVarsFixpointIterator>(
      pointers_fp_iter, effect_summaries, non_overridden_virtuals, cfg);
  used_vars_fp_iter->run(UsedVarsSet());
  return used_vars_fp_iter;
}

std::vector<IRInstruction*> get_dead_instructions(IRCode* code) {
  std::vector<IRInstruction*> result;
  auto fp_iter = analyze(code);
  for (auto it : get_dead_instructions(code, *fp_iter)) {
    result.emplace_back(it->insn);
  }
  return result;
}

TEST_F(UsedVarsTest, simple) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (const v1 0)
      (iput v1 v0 "LFoo;.bar:I")
      (return-void)
    )
  )");
  auto dead_instructions = get_dead_instructions(code.get());
  auto insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::get_type("LFoo;"));
  auto insn2 = (new IRInstruction(OPCODE_CONST))->set_dest(1)->set_literal(0);
  auto insn3 = (new IRInstruction(OPCODE_IPUT))
                   ->set_src(0, 1)
                   ->set_src(1, 0)
                   ->set_field(DexField::get_field("LFoo;.bar:I"));
  EXPECT_THAT(
      dead_instructions,
      UnorderedElementsAre(Pointee(*insn1), Pointee(*insn2), Pointee(*insn3)));
}

TEST_F(UsedVarsTest, join) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (goto :join)
      (:true)
      (new-instance "LBar;")
      (move-result-pseudo-object v1)
      (:join)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");
  auto dead_instructions = get_dead_instructions(code.get());
  auto insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::get_type("LFoo;"));
  auto insn2 = (new IRInstruction(OPCODE_CONST))->set_dest(2)->set_literal(0);
  auto insn3 = (new IRInstruction(OPCODE_IPUT))
                   ->set_src(0, 2)
                   ->set_src(1, 1)
                   ->set_field(DexField::get_field("LFoo;.bar:I"));
  auto insn4 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::get_type("LBar;"));

  EXPECT_THAT(
      dead_instructions,
      UnorderedElementsAre(
          Pointee(*insn1), Pointee(*insn2), Pointee(*insn3), Pointee(*insn4)));
}
