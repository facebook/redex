/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include "Pass.h"
#include "SourceBlocksUtils.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace method_profiles {
class MethodProfiles;
} // namespace method_profiles

// SyntheticBlockCountsPass replaces the boolean-ish per-block SourceBlock `val`
// (today ~1 for a covered block, 0 for an uncovered one) with a synthetic
// per-block EXECUTION COUNT: an estimate of how many times each basic block
// runs. It lets later passes read a block's SourceBlock `val` as a magnitude
// ("this block runs ~50000 times") instead of only a yes/no ("this block was
// seen"), while every existing pass -- all of which only test `val > 0` --
// keeps behaving exactly as before.
//
// The estimate is built from inputs that already exist in the IR and the
// profile: the method's observed `call_count` (how often the whole method ran),
// the boolean coverage already on each block (which blocks ran at all), and the
// CFG shape (branches, loops, exception edges). The full algorithm is written
// up in the ALGORITHM section at the top of SyntheticBlockCounts.cpp.
//
// NO-FUNCTIONAL-CHANGE contract (why enabling the pass is safe):
//   - It writes only `SourceBlock::Val.val`, never `appear100`.
//   - It only ever rewrites a block that is ALREADY covered (`val > 0`); it
//     never turns a 0 into >0 and never turns a covered block into 0. So the
//     set of covered blocks is byte-identical before and after, and every
//     existing `val > 0` decision is unchanged.
//   - A positive result is floored to a small `epsilon`, so a covered block can
//     never underflow below a consumer's threshold.
// Whether the pass runs is controlled by its entry in the Redex pass list.
class SyntheticBlockCountsPass : public Pass {
 public:
  SyntheticBlockCountsPass() : Pass("SyntheticBlockCountsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        // Requires (so it errors if scheduled before InsertSourceBlocksPass)
        // and preserves the source blocks it rewrites.
        {HasSourceBlocks, RequiresAndPreserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  std::string get_config_doc() override;
  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  // Per-method accumulator, summed across the parallel walk.
  struct MethodResult {
    size_t methods_seen{0};
    size_t methods_with_usable_profile{0};
    size_t blocks_written{0};
    size_t solve_fallback_no_profile{0};
    size_t solve_fallback_irreducible{0};
    size_t solve_fallback_entry_is_header{0};
    size_t epsilon_clamped_blocks{0};
    size_t zero_outflow_sinks{0};
    // (method, interaction) sparsity: a pair is "covered" if the
    // method has >=1 val>0 block for that interaction. Uncovered pairs write
    // nothing (support is pinned) -- they are the skippable set a future
    // coverage-gated solve would drop.
    size_t pairs_total{0};
    size_t pairs_covered{0};
    size_t pairs_uncovered{0};

    MethodResult& operator+=(const MethodResult& o) {
      methods_seen += o.methods_seen;
      methods_with_usable_profile += o.methods_with_usable_profile;
      blocks_written += o.blocks_written;
      solve_fallback_no_profile += o.solve_fallback_no_profile;
      solve_fallback_irreducible += o.solve_fallback_irreducible;
      solve_fallback_entry_is_header += o.solve_fallback_entry_is_header;
      epsilon_clamped_blocks += o.epsilon_clamped_blocks;
      zero_outflow_sinks += o.zero_outflow_sinks;
      pairs_total += o.pairs_total;
      pairs_covered += o.pairs_covered;
      pairs_uncovered += o.pairs_uncovered;
      return *this;
    }
  };

  MethodResult process_method(DexMethod* method,
                              cfg::ControlFlowGraph& cfg,
                              const method_profiles::MethodProfiles& profiles,
                              const std::vector<std::string>& inv_slot);

  // Per interaction (index into inv_slot), the number
  // of methods that are covered (>=1 val>0) yet have no usable call_count
  // (absent profile row, or a non-finite or negative call_count; call_count ==
  // 0 is a usable cold anchor) -- the covered-but-unprofiled population the
  // inter-method forward-fill exists to heat. Pure (no pass state); run_pass
  // emits the values as `missing_hit_methods_<interaction>`.
  static std::vector<int64_t> count_missing_hit_methods(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot);

  friend class SyntheticBlockCountsTest;

  float m_handler_cold_factor{0.01f};
  float m_loop_iteration_cap{10.0f};
  float m_default_backedge_prob{0.9f};
  // Floor for a covered block whose solved frequency underflowed. Its only
  // job is to keep such a block strictly > 0; see
  // source_blocks::kMinPositiveCount for why this is no longer sized to
  // dominate a consumer threshold.
  float m_epsilon{source_blocks::kMinPositiveCount};
};
