/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UsedVarsAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "DeadCodeEliminationPass.h"
#include "IRAssembler.h"
#include "RedexTest.h"

namespace ptrs = local_pointers;
namespace uv = used_vars;

using namespace testing;

namespace ptrs = local_pointers;

class UsedVarsTest : public RedexTest {};

std::unique_ptr<uv::FixpointIterator> analyze(
    IRCode& code,
    const ptrs::InvokeToSummaryMap& invoke_to_esc_summary_map,
    const side_effects::InvokeToSummaryMap& invoke_to_eff_summary_map) {
  code.build_cfg(/* editable */ false);
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();

  ptrs::FixpointIterator pointers_fp_iter(cfg, invoke_to_esc_summary_map);
  pointers_fp_iter.run(ptrs::Environment());
  auto used_vars_fp_iter = std::make_unique<uv::FixpointIterator>(
      pointers_fp_iter, invoke_to_eff_summary_map, cfg);
  used_vars_fp_iter->run(uv::UsedVarsSet());

  return used_vars_fp_iter;
}

void optimize(const uv::FixpointIterator& fp_iter, IRCode* code) {
  for (auto it : uv::get_dead_instructions(*code, fp_iter)) {
    code->remove_opcode(it);
  }
}

TEST_F(UsedVarsTest, simple) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
      (const v1 0)
      (iput v1 v0 "LFoo;.bar:I")
      (return-void)
    )
  )");

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) &&
        insn->get_method() == DexMethod::get_method("LFoo;.<init>:()V")) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(UsedVarsTest, join) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (goto :join)

      (:true)
      (new-instance "LBar;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LBar;.<init>:()V")
      (sput v0 "LUnknownClass;.unknownField:I")

      (:join)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && is_init(insn->get_method())) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (goto :join)
      (:true)
      (sput v0 "LUnknownClass;.unknownField:I")
      (:join)
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(UsedVarsTest, noDeleteInit) {
  // Only one branch has a non-escaping object.
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      ; This object is unused and non-escaping; however, since we cannot delete
      ; the `iput` instruction in the join-block below, we cannot delete the
      ; call to Foo.<init>() in this block: writing to an uninitialized object
      ; would be a verification error.
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (goto :join)

      (:true)
      (sget-object "LBar;.bar:LBar;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LBar;.<init>:()V")

      (:join)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");
  auto expected = assembler::to_s_expr(code.get());

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && is_init(insn->get_method())) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}
