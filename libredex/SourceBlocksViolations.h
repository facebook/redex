/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "SourceBlocks.h"

namespace source_blocks {

void fix_chain_violations(cfg::ControlFlowGraph* cfg);

void fix_idom_violations(cfg::ControlFlowGraph* cfg);

void fix_hot_method_cold_entry_violations(cfg::ControlFlowGraph* cfg);

size_t compute_method_violations(const call_graph::Graph& call_graph,
                                 const Scope& scope);

void track_source_block_coverage(ScopedMetrics& sm,
                                 const DexStoresVector& stores);

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

// The canonical name of a violation kind. Used both for trace output and as
// the spelling accepted in the `violations_tracking.violation_kinds` JSON
// configuration.
std::string_view get_violation_name(ViolationsHelper::Violation v);

// Inverse of get_violation_name; std::nullopt for an unrecognized name.
std::optional<ViolationsHelper::Violation> violation_name_to_enum(
    std::string_view name);

// Every name get_violation_name can return, comma-separated, for error
// messages and documentation.
std::string get_violation_names();

} // namespace source_blocks
