/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockOffsetSink.h"

#include <atomic>

#include "ConcurrentContainers.h"

namespace block_offset_sink {
namespace {
std::atomic<bool> s_enabled{false};
// method -> {block anchor MethodItemEntry* -> cfg::Block::id()}. Written once
// per method during the (parallel) pre-lowering capture, then point-read by
// try_sync during the (parallel) sync pass.
InsertOnlyConcurrentMap<const DexMethod*,
                        UnorderedMap<const MethodItemEntry*, uint32_t>>
    s_leaders;
// method -> [(cfg block id, code_unit_offset)]. Written once per method during
// the (parallel) DexOutput sync pass, then point-read single-threaded by the
// dexvt exporter. A sharded concurrent map (not a global mutex) keeps the
// per-method insert off a single lock across ~all methods of an app.
InsertOnlyConcurrentMap<const DexMethod*,
                        std::vector<std::pair<uint32_t, uint32_t>>>
    s_offsets;
} // namespace

void enable() { s_enabled.store(true, std::memory_order_relaxed); }

bool enabled() { return s_enabled.load(std::memory_order_relaxed); }

void set_leaders(const DexMethod* method,
                 UnorderedMap<const MethodItemEntry*, uint32_t> leaders) {
  s_leaders.emplace(method, std::move(leaders));
}

void resolve(
    const DexMethod* method,
    const UnorderedMap<const MethodItemEntry*, uint32_t>& entry_addresses) {
  if (!enabled()) {
    return;
  }
  const auto* leaders = s_leaders.get(method);
  if (leaders == nullptr) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> block_offsets;
  block_offsets.reserve(leaders->size());
  for (const auto& [anchor, blk] : UnorderedIterable(*leaders)) {
    // An anchor lowering removed is simply absent, and its block falls back to
    // no recorded offset.
    auto it = entry_addresses.find(anchor);
    if (it != entry_addresses.end()) {
      block_offsets.emplace_back(blk, it->second);
    }
  }
  if (block_offsets.empty()) {
    return;
  }
  // Each method syncs once, so a plain insert-only emplace suffices; a
  // duplicate (e.g. a resync) keeps the first and is harmless.
  s_offsets.emplace(method, std::move(block_offsets));
}

const std::vector<std::pair<uint32_t, uint32_t>>* get(const DexMethod* method) {
  return s_offsets.get(method);
}

void clear() {
  s_leaders.clear();
  s_offsets.clear();
  s_enabled.store(false, std::memory_order_relaxed);
}

} // namespace block_offset_sink
