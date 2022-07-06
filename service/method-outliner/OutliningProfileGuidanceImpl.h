/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "BigBlocks.h"
#include "Dominators.h"
#include "Lazy.h"
#include "OutliningProfileGuidance.h"

struct ConfigFiles;
class PassManager;

namespace outliner_impl {

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
    std::unordered_set<DexMethod*>* sufficiently_warm_methods,
    std::unordered_set<DexMethod*>* sufficiently_hot_methods);

void propagate_hotness(
    const Scope& scope,
    ConfigFiles& config_files,
    std::unordered_set<DexMethod*>* sufficiently_warm_methods,
    std::unordered_set<DexMethod*>* sufficiently_hot_methods,
    float block_profiles_hits);

outliner::PerfSensitivity parse_perf_sensitivity(const std::string& str);

class CanOutlineBlockDecider {
 private:
  const outliner::ProfileGuidanceConfig& m_config;
  bool m_sufficiently_warm;
  bool m_sufficiently_hot;
  mutable std::unique_ptr<LazyUnorderedMap<cfg::Block*, bool>> m_is_in_loop;
  mutable std::unique_ptr<LazyUnorderedMap<cfg::Block*, boost::optional<float>>>
      m_max_vals;
  mutable std::unique_ptr<dominators::SimpleFastDominators<cfg::GraphInterface>>
      m_dominators;

 public:
  CanOutlineBlockDecider(const outliner::ProfileGuidanceConfig& config,
                         bool sufficiently_warm,
                         bool sufficiently_hot);

  enum class Result {
    CanOutline,
    BlockExceedsThresholds,
    WarmLoop,
    WarmLoopExceedsThresholds,
    WarmLoopNoSourceBlocks,
    Hot,
    HotExceedsThresholds,
    HotNoSourceBlocks,
  };

  Result can_outline_from_big_block(
      const big_blocks::BigBlock& big_block) const;
};

} // namespace outliner_impl
