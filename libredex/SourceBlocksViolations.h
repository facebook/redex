/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "SourceBlocks.h"

namespace method_profiles {
class MethodProfiles;
} // namespace method_profiles

namespace source_blocks {

void fix_chain_violations(ControlFlowGraph* cfg);

void fix_idom_violations(ControlFlowGraph* cfg);

void fix_hot_method_cold_entry_violations(ControlFlowGraph* cfg);

size_t compute_method_violations(const call_graph::Graph& call_graph,
                                 const Scope& scope);

void track_source_block_coverage(ScopedMetrics& sm,
                                 const DexStoresVector& stores);

// [count-integrity] Interprocedural exact-call cap assessment. For each covered
// block that makes an exact single-target call (invoke-static/-direct/-super)
// to a profiled callee, per interaction slot the block's synthetic execution
// count must not exceed the callee's profiled call_count -- a block cannot run
// more often than a method it unconditionally calls. Sound in both the 0/1 and
// count worlds. Read-only; emits `~count~overflow~exact~call` and a max-ratio
// metric.
void track_exact_call_cap_violations(
    ScopedMetrics& sm,
    const DexStoresVector& stores,
    const method_profiles::MethodProfiles& profiles);

struct ViolationsHelper {
  struct ViolationsHelperImpl;
  std::unique_ptr<ViolationsHelperImpl> impl;
  bool track_intermethod_violations{false};
  bool print_all_violations{false};
  bool ignore_undefined{false};

  enum class Violation {
    kHotImmediateDomNotHot = 0,
    kChainAndDom = 1,
    kUncoveredSourceBlocks = 2,
    kHotMethodColdEntry = 3,
    kHotNoHotPred = 4,
    KHotAllChildrenCold = 5,
    kUncoveredThrowDelineatedBlocks = 6,
    ViolationSize = 7,
  };

  ViolationsHelper(Violation v,
                   const Scope& scope,
                   size_t top_n,
                   std::vector<std::string> to_vis,
                   bool track_intermethod_violations,
                   bool print_all_violations,
                   bool ignore_undefined);
  ~ViolationsHelper();

  void process(ScopedMetrics* sm);
  void silence();

  ViolationsHelper(ViolationsHelper&& other) noexcept;
  ViolationsHelper& operator=(ViolationsHelper&& rhs) noexcept;
};

size_t compute(ViolationsHelper::Violation v,
               cfg::ControlFlowGraph& cfg,
               bool ignore_undefined = false);

} // namespace source_blocks
