/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "SourceBlocks.h"

class MetricsSink;
struct ViolationsTrackingConfig;

namespace source_blocks {

void fix_chain_violations(cfg::ControlFlowGraph* cfg);

void fix_idom_violations(cfg::ControlFlowGraph* cfg);

void fix_hot_method_cold_entry_violations(cfg::ControlFlowGraph* cfg);

size_t compute_method_violations(const call_graph::Graph& call_graph,
                                 const Scope& scope);

void track_source_block_coverage(MetricsSink& sm,
                                 const DexStoresVector& stores);

/*
 * Names one source block, or every source block attributed to one method.
 *
 * `method` is matched against `SourceBlock::src`, which is the method a block
 * was attributed to when it was inserted -- not necessarily the one holding it
 * now. Inlining moves blocks between methods and duplicates them into several,
 * so a descriptor identifies a block, not a location, and may resolve to zero,
 * one or many carrier methods.
 */
struct SourceBlockDescriptor {
  const DexString* method{nullptr};
  // std::nullopt matches every block attributed to `method`.
  std::optional<uint32_t> id;

  bool matches(const SourceBlock& sb) const;
  // Round-trips back to the spelling parse_source_block_descriptor accepts.
  std::string str() const;
};

/*
 * Parses `<method>@<id>`, as printed by SourceBlock::show, or a bare
 * `<method>`. The method may be double-quoted, matching show's quoted form,
 * and a trailing value list -- show's `(1:0.5|)` -- is accepted and ignored,
 * so a descriptor copied out of a trace line works verbatim. std::nullopt for
 * anything else, including the ambiguous synthetic id.
 */
std::optional<SourceBlockDescriptor> parse_source_block_descriptor(
    std::string_view descriptor);

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

  struct Params {
    // Every kind here is tracked in one pass over the scope, sharing the
    // per-method CFG normalization and dominator computation.
    //
    // Note that a kind whose counter needs unreachable blocks removed does so
    // for the whole method, so mixing it with a kind that would otherwise see
    // them counts the latter on the normalized CFG too.
    std::vector<Violation> kinds{Violation::kChainAndDom};
    // How many of the worst-regressing methods to report per kind.
    size_t top_n{10};
    // Methods whose violations are logged in full.
    std::vector<std::string> to_vis;
    // When non-empty, only the methods carrying these source blocks are
    // tracked, and each of them is reported even with no violations.
    std::vector<SourceBlockDescriptor> targets;
    bool track_intermethod_violations{false};
    bool print_all_violations{false};
    bool ignore_undefined{false};
  };

  ViolationsHelper(Params params, const Scope& scope);
  ~ViolationsHelper();

  // A null sink reports nothing; the destructor uses that to fall back to
  // trace-only output.
  void process(MetricsSink* sm);
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

/*
 * Per-pass violations tracking, as driven by the `violations_tracking` global
 * configuration.
 *
 * Construct one instance per Redex run. Each `Handler` then brackets a single
 * pass: its constructor snapshots the current per-method violation counts, and
 * its destructor recounts, diffs against the snapshot, and reports the
 * increase under a `~violation~tracking` scope of the given metrics sink.
 */
class ViolationsTracking {
 public:
  explicit ViolationsTracking(const ViolationsTrackingConfig& config);

  class Handler {
   public:
    Handler(const ViolationsTracking& tracking,
            MetricsSink* sink,
            const DexStoresVector& stores);
    ~Handler();

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;

    Handler(Handler&& other) noexcept;
    Handler& operator=(Handler&& rhs) noexcept;

   private:
    MetricsSink* m_sink;
    std::unique_ptr<ViolationsHelper> m_vh;
  };

  // std::nullopt when tracking is not enabled, so that a caller can hold the
  // result unconditionally.
  std::optional<Handler> maybe_track(MetricsSink* sink,
                                     const DexStoresVector& stores) const;

 private:
  const ViolationsTrackingConfig& m_config;
  std::vector<ViolationsHelper::Violation> m_violations;
  std::vector<SourceBlockDescriptor> m_targets;
};

} // namespace source_blocks
