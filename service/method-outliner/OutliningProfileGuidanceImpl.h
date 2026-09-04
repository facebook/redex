/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include <optional>
#include <utility>

#include "BigBlocks.h"
#include "ConcurrentContainers.h"
#include "DeterministicContainers.h"
#include "OutliningProfileGuidance.h"

struct ConfigFiles;
class PassManager;

namespace outliner_impl {

void get_throughput_interactions(
    ConfigFiles& config_files,
    const outliner::ProfileGuidanceConfig& config,
    UnorderedSet<size_t>* throughput_interaction_indices,
    UnorderedSet<std::string>* throughput_interaction_ids);

////////////////////////////////////////////////////////////////////////////////
// gather_sufficiently_warm_and_hot_methods
////////////////////////////////////////////////////////////////////////////////

// We'll look around the provided configuration information to identify hot and
// warm methods. The preferred way is now to use "method profiles". We look at
// each interaction. If a method appears in at least 1% of the samples, then...
// - If the method is invoked at least 10 times on average, we won't outline
//   from it at all (truly "hot")
// - If the method is invoked less often ("at least once", otherwise it wouldn't
//   appear in the method profiles), then we won't outline from any of its loops
//   ("warm" code)
//
// The actual thresholds are configurable.
//
// The intention here is to avoid outlining any code snippet that runs many
// times, in which case the call overhead might become significant. Otherwise,
// if it is called only rarely (0 to 9 times), then any added CPU overhead might
// be made up by the I/O savings due to reduced code size.
//
// When method profiles are completely unavailable, we can use cold-start
// classes to identify warm code.
void gather_sufficiently_warm_and_hot_methods(
    const Scope& scope,
    ConfigFiles& config_files,
    PassManager& mgr,
    const outliner::ProfileGuidanceConfig& config,
    const UnorderedSet<std::string>& throughput_interaction_ids,
    UnorderedSet<DexMethod*>* throughput_methods,
    UnorderedSet<DexMethod*>* sufficiently_warm_methods,
    UnorderedSet<DexMethod*>* sufficiently_hot_methods);

void propagate_hotness(const Scope& scope,
                       ConfigFiles& config_files,
                       UnorderedSet<DexMethod*>* sufficiently_warm_methods,
                       UnorderedSet<DexMethod*>* sufficiently_hot_methods,
                       float block_profiles_hits);

outliner::PerfSensitivity parse_perf_sensitivity(const std::string& str);

// Answers "may the outliner take code out of this big block", for every method
// in a pass.
//
// One instance serves the whole pass, so its memo tables outlive the blocks
// they describe: the outliner rewrites each dex's CFGs as it finishes with
// them. Entries are therefore keyed by something that stays meaningful after
// the block is gone rather than by the block's address -- see `BlockKey`. That
// is what makes a pass-lifetime cache answer the same way regardless of how the
// allocator reuses memory, which matters here because the answers decide what
// gets outlined.
//
// The type is immovable on purpose: callers hold it by reference for the length
// of the pass, and a cache that could be relocated under them is a cache whose
// identity is harder to reason about than the one property it needs.
class OutlineabilityContext {
 public:
  OutlineabilityContext(
      const outliner::ProfileGuidanceConfig& config,
      UnorderedSet<size_t> throughput_interaction_indices,
      const UnorderedSet<DexMethod*>& throughput_methods,
      const UnorderedSet<DexMethod*>& sufficiently_warm_methods,
      const UnorderedSet<DexMethod*>& sufficiently_hot_methods);

  OutlineabilityContext(const OutlineabilityContext&) = delete;
  OutlineabilityContext& operator=(const OutlineabilityContext&) = delete;

  enum class Result {
    CanOutline,
    BlockExceedsThresholds,
    WarmLoop,
    WarmLoopExceedsThresholds,
    WarmLoopNoSourceBlocks,
    Throughput,
    ThroughputExceedsThresholds,
    ThroughputNoSourceBlocks,
    Hot,
    HotExceedsThresholds,
    HotNoSourceBlocks,
  };

  Result can_outline_from_big_block(
      DexMethod* method, const big_blocks::BigBlock& big_block) const;

 private:
  // A block identifies itself only for as long as it is alive: outlining
  // rewrites the CFGs it has finished with, and a freed block's address is free
  // to name a different block afterwards. A cache that outlives one method
  // therefore cannot key on the pointer -- it would answer for whichever block
  // happened to occupy the address first, and which block that is depends on
  // the allocator rather than on the code. Block ids are unique within a
  // method's CFG and methods outlive the pass, so the pair is stable for as
  // long as the entry is reachable.
  using BlockKey = std::pair<const DexMethod*, cfg::BlockId>;

  // Every generator is a pure function of the block, so two threads racing on a
  // key agree by construction.
  bool is_in_loop(const DexMethod* method, cfg::Block* block) const;
  bool is_throughput(const DexMethod* method, cfg::Block* block) const;
  std::optional<float> max_val(const DexMethod* method,
                               cfg::Block* block) const;

  const outliner::ProfileGuidanceConfig& m_config;
  const UnorderedSet<size_t> m_throughput_interaction_indices;
  const UnorderedSet<DexMethod*>& m_throughput_methods;
  const UnorderedSet<DexMethod*>& m_sufficiently_warm_methods;
  const UnorderedSet<DexMethod*>& m_sufficiently_hot_methods;

  mutable InsertOnlyConcurrentMap<BlockKey, bool, boost::hash<BlockKey>>
      m_is_in_loop;
  mutable InsertOnlyConcurrentMap<BlockKey, bool, boost::hash<BlockKey>>
      m_is_throughput;
  mutable InsertOnlyConcurrentMap<BlockKey,
                                  std::optional<float>,
                                  boost::hash<BlockKey>>
      m_max_vals;
};

} // namespace outliner_impl
