/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "DeterministicContainers.h"

class DexMethod;
struct MethodItemEntry;

/*
 * Gated, thread-safe sink that gives every cfg block its final shipped-DEX
 * code-unit offset, in two phases:
 *
 *  1) Capture (before the DexOutput sync pass, while the pre-lowering CFG is
 *     available): the producer registers each block's ANCHOR -- its first
 *     MethodItemEntry -> cfg::Block::id() -- via set_leaders(). A block's first
 *     entry splices through CFG teardown and survives in-place instruction
 *     lowering as the same object, so its pointer identity is a stable key.
 *  2) Resolve (IRCode::sync): once the final DEX layout is known, try_sync
 *     looks up each anchor in its entry->code-unit-offset map and record()s the
 *     resulting (cfg::Block::id() -> code-unit offset) pairs.
 *
 * Anchoring on the block's first entry (rather than only a SourceBlock) yields
 * an offset for every block WITH CODE -- source-blocked or not -- so a block's
 * three dexvt levels (IR / true-DEX / native) slice to exactly the same region.
 * A codeless block (empty, or a source-block-only fallthrough) is skipped: it
 * has no DEX and its offset would alias the next block's.
 *
 * Disabled by default, and the disabled path is free: IRCode::sync does one
 * relaxed atomic load per method and skips the resolve pass wholesale, so a
 * build that does not ask for dexvt pays nothing per instruction. A producer
 * (the dexvt exporter, or a unit test) calls enable() before the DexOutput sync
 * pass and drains the recorded offsets afterward with get().
 */
namespace block_offset_sink {

void enable();
bool enabled();

// Capture-time: register a method's block anchors (each block's first
// MethodItemEntry -> cfg::Block::id()). Call before the sync pass.
void set_leaders(const DexMethod* method,
                 UnorderedMap<const MethodItemEntry*, uint32_t> leaders);

// Sync-time: join a method's anchors against the final MethodItemEntry ->
// code-unit-address map that IRCode::sync hands back, and record the resulting
// (cfg::Block::id(), code_unit_offset) pairs -- one per block whose anchor was
// still present in the final layout. A no-op when the sink is disabled, so the
// caller pays nothing beyond one relaxed atomic load.
void resolve(
    const DexMethod* method,
    const UnorderedMap<const MethodItemEntry*, uint32_t>& entry_addresses);

// The recorded pairs for a method, or nullptr if none were recorded. Valid
// until the next clear(); call after the (parallel) sync pass has finished.
const std::vector<std::pair<uint32_t, uint32_t>>* get(const DexMethod* method);

// Drop all recorded state and disable (test isolation / between runs).
void clear();

} // namespace block_offset_sink
