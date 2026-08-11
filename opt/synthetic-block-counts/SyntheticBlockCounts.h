/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ConcurrentContainers.h"
#include "DexClass.h"
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
//
// Two modes, selected by the `intermethod` config knob:
//   - PER-METHOD (default): each method is estimated on its own, anchored to
//   its
//     own `call_count`. See SyntheticBlockCounts.cpp, ALGORITHM section A.
//   - INTER-METHOD (`intermethod=true`): counts also flow across method
//     boundaries along the call graph, so a method with no profile of its own
//     is still heated by its hot callers. See ALGORITHM section B.
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

  // Result of the clamp post-pass (for metrics + tests).
  struct ClampStats {
    size_t clamp_vals_lowered{0}; // covered SourceBlock vals the clamp lowered
    double clamp_val_removed_total{0.0}; // sum of (old-new) over lowered vals
    size_t clamp_exact_invokes_seen{0}; // exact invokes inspected
    size_t clamp_unresolved_invokes{0}; // exact opcode, target not resolvable
    size_t clamp_unprofiled_callees{0}; // resolved callee, no row (per i)
    size_t clamp_throw_gated_invokes{0}; // invoke skipped: a may-throw precedes
                                         // it
  };

  // Scope-scoped core (no PassManager), directly drivable from
  // unit tests: cap each covered block's `val` at the sound min-profiled-callee
  // bound (min over the block's distinct exact callees of
  // floor(call_count / in-block multiplicity)), floored at epsilon. Only
  // lowers; no re-solve. The hard soundness backstop. Runs on whatever the
  // producer wrote (per-method OR inter-method).
  ClampStats clamp_post_pass_core(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot);

  // Thin metric wrapper around clamp_post_pass_core.
  void clamp_post_pass(const Scope& scope,
                       const method_profiles::MethodProfiles& profiles,
                       const std::vector<std::string>& inv_slot,
                       PassManager& mgr);

  // Result of the re-flow post-pass (for metrics + tests).
  struct ReflowStats {
    size_t reflow_blocks_capped{0}; // blocks in an SCC whose flow was routed
                                    // down
    double reflow_sink_spill_total{0.0}; // flow no successor could absorb
    size_t reflow_methods_spilled{0}; // methods with any sink spill
    ReflowStats& operator+=(const ReflowStats& o) {
      reflow_blocks_capped += o.reflow_blocks_capped;
      reflow_sink_spill_total += o.reflow_sink_spill_total;
      reflow_methods_spilled += o.reflow_methods_spilled;
      return *this;
    }
  };

  // Scope-scoped core (no PassManager), directly drivable
  // from unit tests: the two-sweep capacity DP on the SCC condensation. Per
  // method, per interaction, anchored on the WRITTEN entry count, it routes
  // flow away from over-capped blocks toward siblings with headroom (entry
  // pinned; a block's own value is never truncated -- the clamp is the hard
  // backstop) and overwrites covered blocks with the routed frequency. Runs on
  // whatever the producer wrote (per-method OR inter-method).
  ReflowStats reflow_post_pass_core(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot);

  // Thin metric wrapper around reflow_post_pass_core.
  void reflow_post_pass(const Scope& scope,
                        const method_profiles::MethodProfiles& profiles,
                        const std::vector<std::string>& inv_slot,
                        PassManager& mgr);

  // Summary of what the inter-method engine did (for metrics + tests).
  struct InterStats {
    // PASS C runs once per (method, interaction), so these four count
    // (method, interaction) pairs, NOT distinct methods -- a method pinned in
    // every slot contributes `inv_slot.size()` to `pairs_pinned`. Use
    // `forward_methods_mutated` below for a distinct-method count.
    size_t pairs_pinned{0};
    size_t pairs_forward_filled{0};
    size_t pairs_skipped{0};
    size_t pairs_hit_ceiling{0}; // value limited by the max_scale ceiling
    size_t blocks_written{0};
    // blocks written to forward-filled (unprofiled) methods only == the delta
    // over the per-method path (per-method leaves unprofiled methods boolean).
    size_t forward_blocks_written{0};
    size_t methods_not_in_graph{0}; // profiled-but-unreachable (pinned anyway)
    // (method, interaction) sparsity (see run_intermethod_core).
    size_t pairs_total{0};
    size_t pairs_covered{0};
    size_t pairs_uncovered{0};
    // engine cost + parity.
    size_t shape_solves{0}; // actual solve_block_freq calls (~= RelevanceSet)
    size_t shape_cache_misses{0}; // canary: covered+scale>0 not solved. MUST be
                                  // 0
    size_t zero_outflow_sinks{0}; // restored from the discarded solve out-param
    size_t irreducible{0}; // restored from the discarded solve out-param
    size_t relevance_set_size{0}; // summed over interactions
    size_t shape_cache_entries{0}; // peak covered-shape cache size
    size_t sweeps_total{0}; // PASS B sweeps summed over interactions
    uint32_t sweeps_max{0}; // max sweeps any single interaction needed
    size_t interactions_hit_cap{0}; // interactions that exhausted max_sweeps
    size_t methods_unconverged{0}; // methods still moving on the final sweep,
                                   // summed over non-converged interactions
    double final_rel_max{0.0}; // worst final-sweep relative change, any slot
    // distinct forward-filled methods with >=1 block written (deduped over
    // interaction slots), and the biggest few by dex size -- the latter emitted
    // as name-carrying metrics (key = rank + method name, value = dex size).
    size_t forward_methods_mutated{0};
    std::vector<std::pair<const DexMethod*, uint32_t>> top_forward_mutated;
    double p1_ms{0.0}; // PASS 0a wall time (driving thread)
    double phase_b_ms{0.0}; // PASS B wall time
    double phase_c_ms{0.0}; // PASS C wall time
  };

  // Inter-method engine entry point. Runs only when `m_intermethod`
  // is set; otherwise `run_pass` uses the per-method `process_method` path.
  void run_intermethod(DexStoresVector& stores,
                       const method_profiles::MethodProfiles& profiles,
                       const std::vector<std::string>& inv_slot,
                       PassManager& mgr);

  // Scope-scoped core of the inter-method engine (no PassManager / stores
  // dependency), so it is directly drivable from unit tests.
  InterStats run_intermethod_core(
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

  // Tier-1 callsite-count clamp: standalone post-pass over
  // BOTH producer paths, capping each covered block at the min profiled
  // call_count over its exact (invoke-static/-direct/-super) callees (divided
  // by in-block multiplicity). Sound, deterministic, no call graph. UNBOUND --
  // there is no config knob; it is a friend-test-only toggle that defaults on
  // (the whole pass is gated by the pass list).
  bool m_callsite_clamp{true};

  // Tier-1 callsite re-flow: the flow-aware variant of the
  // clamp. A two-sweep capacity DP on the SCC condensation routes flow away
  // from over-capped blocks toward siblings with headroom (no iteration,
  // deterministic), then the clamp truncates as the hard backstop. UNBOUND
  // friend-test-only toggle; defaults on (the whole pass is gated by the pass
  // list).
  bool m_callsite_reflow{true};

  // Inter-method engine knobs.
  bool m_intermethod{false};
  uint32_t m_intermethod_max_sweeps{32};
  // Widening-to-hi safety net multiplier: every forward-filled scale is capped
  // at `factor * max(usable call_count)` per interaction, so a recursion/SCC
  // cycle is provably bounded by a data-derived ceiling (then tightened by the
  // step-4 backward callee bound).
  float m_intermethod_max_scale_factor{10.0f};
  // Relative convergence tolerance for the Gauss-Seidel solve.
  float m_intermethod_converge_eps{1e-4f};

  // Per-method bitmask of the interaction slots the producer actually wrote, so
  // the callsite clamp only caps producer-synthesized counts and never lowers
  // the original boolean vals of methods/interactions the producer left
  // untouched. Bit i set => producer wrote `method` for inv_slot[i] (i < 64).
  // Populated by process_method / run_intermethod_core, read by
  // clamp_post_pass_core; cleared at the top of run_pass.
  ConcurrentMap<const DexMethod*, uint64_t> m_producer_written;
};
