/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BlockOffsetSink.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRList.h"
#include "IROpcode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"

namespace {

class BlockOffsetSinkTest : public RedexTest {};

// With the sink enabled, IRCode::sync must record, for EVERY cfg block, the
// code-unit offset of that block's first instruction in the FINAL DexCode --
// the shipped-DEX offset dexdump prints -- not only for source-blocked blocks.
// We build a method whose blocks mix source-blocked and non-source-blocked
// (a forward branch + a goto), capture each block's first-OPCODE anchor from
// the pre-lowering CFG exactly as DexVt does, run a CFG round-trip (as an
// intervening pass would), lower, then sync and check the recorded (blk ->
// offset) against an independent prefix-sum of the final DexInstruction sizes.
// Anchoring the first opcode -- not &*begin() -- is what keeps a source-blocked
// block's leading MFLOW_SOURCE_BLOCK from stealing (and then losing) the anchor
// across passes.
TEST_F(BlockOffsetSinkTest, blockOffsetsMatchFinalDexLayoutForEveryBlock) {
  // Blocks 0 and :left lead with a SourceBlock (so &*begin() would be the SB,
  // an anchor that does not survive to sync); the fallthrough and :end blocks
  // lead with an opcode -- so this exercises both kinds and proves
  // non-source-blocked blocks are captured too.
  const char* code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :left)

      (const v1 1)
      (goto :end)

      (:left)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0))
      (const v1 2)

      (:end)
      (return-void)
    )
  )";
  auto* method = assembler::method_from_string(
      std::string("(method (public static) \"LFoo;.bar:()V\" ") + code_str +
      ")");
  auto* ir = method->get_code();

  // Capture each block's anchor (its first OPCODE MethodItemEntry -> cfg block
  // id) from the CFG, as DexVt does pre-lowering. An instruction anchor splices
  // through clear_cfg and survives in-place lowering as the same object. We
  // must NOT anchor &*b->begin(): for a source-blocked block that is a leading
  // MFLOW_SOURCE_BLOCK, whose MIE does not survive to sync.
  ir->build_cfg();
  UnorderedMap<const MethodItemEntry*, uint32_t> leaders;
  size_t n_blocks_with_code = 0;
  bool saw_source_blocked = false; // a block whose first entry is NOT an opcode
  for (auto* b : ir->cfg().blocks()) {
    const MethodItemEntry* first_op = nullptr;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_OPCODE) {
        first_op = &mie;
        break;
      }
    }
    if (first_op == nullptr) {
      continue; // no DEX to show; its offset would alias the next block's
    }
    if (b->begin()->type != MFLOW_OPCODE) {
      saw_source_blocked = true; // &*b->begin() would be the wrong anchor here
    }
    leaders.emplace(first_op, static_cast<uint32_t>(b->id()));
    ++n_blocks_with_code;
  }
  // The fixture must actually exercise a source-blocked block -- otherwise the
  // test could not distinguish anchoring the first opcode from anchoring
  // begin().
  ASSERT_TRUE(saw_source_blocked);
  ir->clear_cfg();

  // Simulate a pass running between DexVt's capture and the final sync: a CFG
  // round-trip. Instruction anchors survive it; a leading-SourceBlock anchor
  // would be lost -- which is why the block's first opcode is the anchor.
  ir->build_cfg();
  ir->clear_cfg();

  instruction_lowering::lower(method);

  // Oracle: walk the lowered IR (anchors survive) to map each block id to the
  // index of its first dex instruction. sync() consumes the IR, so run first.
  std::unordered_map<uint32_t, size_t> blk_to_idx;
  {
    size_t idx = 0;
    for (const auto& mie : *ir) {
      if (auto it = leaders.find(&mie); it != leaders.end()) {
        blk_to_idx[it->second] = idx;
      }
      if (mie.type == MFLOW_DEX_OPCODE) {
        ++idx;
      }
    }
  }
  ASSERT_EQ(blk_to_idx.size(), n_blocks_with_code);

  block_offset_sink::clear();
  block_offset_sink::enable();
  block_offset_sink::set_leaders(method, leaders);
  auto dex_code = ir->sync(method);

  // Independent oracle: prefix-sum the FINAL DexInstruction sizes to get the
  // code-unit offset at each instruction index (this is the shipped-DEX
  // layout).
  const auto& insns = dex_code->get_instructions();
  std::vector<uint32_t> offset_at_index;
  offset_at_index.reserve(insns.size() + 1);
  uint32_t running = 0;
  for (auto* insn : insns) {
    offset_at_index.push_back(running);
    running += static_cast<uint32_t>(insn->size());
  }
  offset_at_index.push_back(running); // sentinel for an anchor after the last
                                      // insn

  const auto* recorded = block_offset_sink::get(method);
  ASSERT_NE(recorded, nullptr);

  // (1) Every block with code got an offset -- source-blocked or not.
  EXPECT_EQ(recorded->size(), n_blocks_with_code);

  // (2) Each recorded offset equals that block's first-instruction offset.
  std::unordered_map<uint32_t, uint32_t> recorded_map;
  for (const auto& [blk, off] : *recorded) {
    recorded_map[blk] = off;
  }
  for (const auto& [blk, idx] : blk_to_idx) {
    ASSERT_LE(idx, insns.size());
    auto it = recorded_map.find(blk);
    ASSERT_TRUE(it != recorded_map.end())
        << "cfg block " << blk << " missing from recorded offsets";
    EXPECT_EQ(it->second, offset_at_index[idx])
        << "cfg block " << blk << " code-unit offset mismatch";
  }

  block_offset_sink::clear();
}

// The ENTRY block must get an offset for INSTANCE methods too, not only static
// ones. A non-static method's leading `this` load-param is erase_and_dispose()d
// by instruction_lowering's check_load_params, so a block anchored on it has no
// anchor at sync and drops out -- taking `B0:` out of every block-aligned
// DEX/native view. Anchoring the first DEX-EMITTING opcode keeps the anchor;
// load-params emit no DEX, so the recorded start_cu is unchanged.
//
// The four quadrants matter independently: a STATIC method with params also
// leads with load-params, but remove_opcode merely retypes those in place to
// MFLOW_FALLTHROUGH, so the pointer stays valid and the static rows pass either
// way. Only the instance rows regress when the skip is removed.

struct AnchorResult {
  size_t n_blocks_with_dex{0};
  size_t n_recorded{0};
  bool entry_recorded{false};
  bool has_offset_zero{false};
};

// Drive the real capture -> pass round-trip -> lower -> sync order, choosing
// anchors exactly as DexVt::capture_pre_lowering does.
AnchorResult capture_lower_sync(DexMethod* method) {
  AnchorResult r;
  auto* ir = method->get_code();

  ir->build_cfg();
  UnorderedMap<const MethodItemEntry*, uint32_t> leaders;
  auto* entry_block = ir->cfg().entry_block();
  auto entry_blk = static_cast<uint32_t>(entry_block->id());
  for (auto* b : ir->cfg().blocks()) {
    const MethodItemEntry* first_op = nullptr;
    for (const auto& mie : *b) {
      // Mirrors DexVt.cpp: skip load-params, they emit no DEX (and a
      // non-static method's `this` load-param does not survive lowering).
      if (mie.type == MFLOW_OPCODE &&
          !opcode::is_an_internal(mie.insn->opcode())) {
        first_op = &mie;
        break;
      }
    }
    if (first_op == nullptr) {
      continue;
    }
    EXPECT_FALSE(opcode::is_an_internal(first_op->insn->opcode()))
        << "an internal opcode emits no DEX, so anchoring on one aliases the "
           "next block's start_cu";
    leaders.emplace(first_op, static_cast<uint32_t>(b->id()));
    ++r.n_blocks_with_dex;
  }
  ir->clear_cfg();

  // An intervening pass between capture and the final sync.
  ir->build_cfg();
  ir->clear_cfg();

  instruction_lowering::lower(method);

  block_offset_sink::clear();
  block_offset_sink::enable();
  block_offset_sink::set_leaders(method, leaders);
  auto dex_code = ir->sync(method);

  const auto* recorded = block_offset_sink::get(method);
  if (recorded != nullptr) {
    r.n_recorded = recorded->size();
    for (const auto& [blk, off] : *recorded) {
      if (blk == entry_blk) {
        r.entry_recorded = true;
      }
      if (off == 0) {
        r.has_offset_zero = true;
      }
    }
  }
  block_offset_sink::clear();
  return r;
}

void expect_entry_block_anchored(const char* label, DexMethod* method) {
  AnchorResult r = capture_lower_sync(method);
  EXPECT_GT(r.n_blocks_with_dex, 1u) << label << ": fixture has no branching";
  EXPECT_EQ(r.n_recorded, r.n_blocks_with_dex)
      << label << ": not every block with DEX got an offset";
  EXPECT_TRUE(r.entry_recorded)
      << label << ": the ENTRY block has no recorded offset";
  EXPECT_TRUE(r.has_offset_zero)
      << label << ": no block is anchored at code-unit 0";
}

TEST_F(BlockOffsetSinkTest, entryBlockIsAnchoredForStaticAndInstanceMethods) {
  // Params must occupy the HIGHEST registers of the frame (check_load_params).
  expect_entry_block_anchored("static/no-params",
                              assembler::method_from_string(R"(
    (method (public static) "LSpike;.sn:()V"
      (
        (const v0 0)
        (if-eqz v0 :left)
        (const v1 1)
        (goto :end)
        (:left)
        (const v1 2)
        (:end)
        (return-void)
      )
    )
  )"));

  expect_entry_block_anchored("static/params", assembler::method_from_string(R"(
    (method (public static) "LSpike;.sa:(II)V"
      (
        (load-param v1)
        (load-param v2)
        (if-eqz v1 :left)
        (const v0 1)
        (goto :end)
        (:left)
        (const v0 2)
        (:end)
        (return-void)
      )
    )
  )"));

  // The regressing cases: a non-static method always has a `this` load-param,
  // even with an empty signature.
  expect_entry_block_anchored("instance/no-params",
                              assembler::method_from_string(R"(
    (method (public) "LSpike;.in:()V"
      (
        (load-param-object v2)
        (const v0 0)
        (if-eqz v0 :left)
        (const v1 1)
        (goto :end)
        (:left)
        (const v1 2)
        (:end)
        (return-void)
      )
    )
  )"));

  expect_entry_block_anchored("instance/params",
                              assembler::method_from_string(R"(
    (method (public) "LSpike;.ia:(II)V"
      (
        (load-param-object v1)
        (load-param v2)
        (load-param v3)
        (if-eqz v2 :left)
        (const v0 1)
        (goto :end)
        (:left)
        (const v0 2)
        (:end)
        (return-void)
      )
    )
  )"));
}

// A block whose first DEX-emitting opcode is a `check-cast` needing a register
// move. instruction_lowering::lower_check_cast splices a NEW `move` MIE in
// BEFORE the cast (InstructionLowering.cpp: `code->insert_before(it, dex_mov)`)
// whenever the move-result-pseudo's dest differs from the cast's src -- so the
// block's real first instruction is that move, and the anchored cast sits one
// code unit later. `store.block_spans` treats each block's start as exactly
// closing the previous one, so an anchor that is late does not merely lose
// precision: it hands the inserted move to the PRECEDING block.
//
// This shape is ordinary Kotlin (an `else` arm opening with a cast). Every
// other fixture here is const/if-eqz/goto/return, none of which lowering
// re-writes, which is why nothing else covers it.
TEST_F(BlockOffsetSinkTest,
       blockLedByALoweredCheckCastIsAnchoredAtItsFirstInstruction) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LCc;.m:(Ljava/lang/Object;)V"
      (
        (load-param-object v1)
        (if-eqz v1 :cast)
        (const v0 0)
        (goto :end)
        (:cast)
        (check-cast v1 "Ljava/lang/String;")
        (move-result-pseudo-object v0)
        (:end)
        (return-void)
      )
    )
  )");
  auto* ir = method->get_code();

  ir->build_cfg();
  UnorderedMap<const MethodItemEntry*, uint32_t> leaders;
  uint32_t cast_blk = std::numeric_limits<uint32_t>::max();
  for (auto* b : ir->cfg().blocks()) {
    const MethodItemEntry* first_op = nullptr;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_OPCODE &&
          !opcode::is_an_internal(mie.insn->opcode())) {
        first_op = &mie;
        break;
      }
    }
    if (first_op == nullptr) {
      continue;
    }
    leaders.emplace(first_op, static_cast<uint32_t>(b->id()));
    if (first_op->insn->opcode() == OPCODE_CHECK_CAST) {
      cast_blk = static_cast<uint32_t>(b->id());
    }
  }
  ASSERT_NE(cast_blk, std::numeric_limits<uint32_t>::max())
      << "fixture must have a block led by a check-cast";
  ir->clear_cfg();

  instruction_lowering::lower(method);

  block_offset_sink::clear();
  block_offset_sink::enable();
  block_offset_sink::set_leaders(method, leaders);
  auto dex_code = ir->sync(method);

  // Independent oracle, layout-order agnostic: find the emitted check-cast; the
  // block begins at the `move` lowering spliced in immediately before it.
  const auto& insns = dex_code->get_instructions();
  std::vector<uint32_t> offset_at_index;
  uint32_t running = 0;
  for (auto* insn : insns) {
    offset_at_index.push_back(running);
    running += static_cast<uint32_t>(insn->size());
  }
  size_t cast_idx = insns.size();
  for (size_t i = 0; i < insns.size(); ++i) {
    if (insns[i]->opcode() == DOPCODE_CHECK_CAST) {
      cast_idx = i;
      break;
    }
  }
  ASSERT_LT(cast_idx, insns.size()) << "fixture must emit a check-cast";
  ASSERT_GT(cast_idx, 0u) << "fixture must not start with the cast";
  ASSERT_TRUE(dex_opcode::is_move(insns[cast_idx - 1]->opcode()))
      << "fixture did not trigger the check-cast move insertion";
  const uint32_t block_start = offset_at_index[cast_idx - 1];

  const auto* recorded = block_offset_sink::get(method);
  ASSERT_NE(recorded, nullptr);
  uint32_t got = std::numeric_limits<uint32_t>::max();
  for (const auto& [blk, off] : *recorded) {
    if (blk == cast_blk) {
      got = off;
    }
  }
  ASSERT_NE(got, std::numeric_limits<uint32_t>::max())
      << "cast-led block has no recorded offset";
  EXPECT_EQ(got, block_start)
      << "block start is late by " << (got - block_start)
      << " code unit(s): the move lowering inserted for the check-cast falls "
         "outside the block it belongs to";

  block_offset_sink::clear();
}

} // namespace
