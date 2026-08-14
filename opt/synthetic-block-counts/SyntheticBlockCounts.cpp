/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SyntheticBlockCounts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "LoopInfo.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

// ============================================================================
// ALGORITHM
// ============================================================================
// This pass turns the boolean coverage on SourceBlocks into per-block execution
// COUNTS. The entry block of a method is assumed to run `call_count` times (a
// number from the profile); every other block runs some fraction or multiple of
// that, decided by the branches and loops between it and the entry.
//
// ----------------------------------------------------------------------------
// SECTION A -- Per-method estimate (the default path; `process_method`)
// ----------------------------------------------------------------------------
// The per-block frequencies are computed with a standard two-step control-flow
// frequency propagation (the "Wu-Larus" scheme; Wu & Larus, "Static Branch
// Frequency and Program Profile Analysis," MICRO-27, 1994):
//
// The two-step split -- edge probabilities first, then block frequencies --
// mirrors the architecture of LLVM's `BranchProbabilityInfo` /
// `BlockFrequencyInfo` (`BlockFrequencyInfoImpl`): assign edge probabilities,
// then derive relative block frequencies. This is an architectural analogy, not
// a port -- LLVM carries far richer static branch heuristics and general
// cyclic-region handling, whereas here SourceBlock coverage supplies the
// primary support constraint and reducible loops use a bounded geometric
// approximation.
//
// STEP 1 -- edge probabilities. For each block, split its outgoing flow across
// its successor edges, layered:
//   * structural default: one successor gets all the flow; a multi-way split
//     (if/else or switch) divides evenly across its successors. An if/else is
//     NOT biased by branch direction -- which arm is the fall-through vs. the
//     explicit target is an invertible compiler artifact that carries no
//     signal.
//   * appear100 nudge: multiply an edge's weight by the target block's
//     `appear100` (the fraction of profiled runs the target appeared in), so a
//     rarely-seen successor gets less flow.
//   * exception edges (thrown-exception successors) get a tiny weight
//     (`handler_cold_factor`), since handlers rarely run.
//   * coverage pin: a successor whose boolean coverage is 0 for this
//   interaction
//     (it never ran) gets NO flow. This keeps the estimate consistent with what
//     was actually observed.
// Each block's out-edge weights are then normalized to sum to 1.
//
// The exception-cold default above is a static prior in the tradition of Ball &
// Larus, "Branch Prediction for Free," PLDI 1993 -- a guess, not a measurement.
// Among ordinary covered successors we make NO directional guess: SourceBlock
// support constrains WHICH successors may receive flow, and where it does not
// reveal the split we divide uniformly (maximum entropy) rather than invent a
// bias the edge encoding cannot justify.
//
// STEP 2 -- forward propagation. Visit blocks in reverse-post-order (each block
// after all of its non-loop predecessors). A block's frequency is the sum over
// its incoming forward edges of (source frequency * edge probability), starting
// from entry = `call_count`.
//   * Loops: a reducible (well-formed) loop header is amplified by the
//     closed-form geometric factor 1/(1 - p_back), where p_back is the
//     back-edge probability, capped at `loop_iteration_cap` -- so an estimated
//     loop can multiply a block's count by at most that cap.
//   * Odd loop shapes (irreducible / multi-entry) are treated as plain merges
//     (no amplification) rather than guessed at.
//
// WRITE-BACK. The solved frequency is written to `val` only for blocks already
// covered (`val > 0`); 0 and NaN are left untouched. The result is floored to
// `epsilon` (kept strictly > 0) and kept as a float. `appear100` is never
// written. A block with no profile for an interaction keeps its boolean value.
//
// EXAMPLE. A method called 1000 times, with a covered if/else and a covered
// loop inside one arm:
//     entry             -> 1000
//     if-then / if-else -> ~500 each (0.5 split)
//     loop body         -> ~500 * (loop factor, capped)
// If the else-arm block had boolean coverage 0 (never observed), it gets no
// flow and its `val` stays 0.
//
// LIMITATION -- this estimate is purely LOCAL. Each method is anchored to its
// OWN `call_count`, with no reconciliation across method boundaries. So the
// count this pass gives a caller's invoke block (an estimate of how many times
// that call site fires) and the count it independently gives the callee method
// (anchored to the callee's own `call_count`) can disagree -- the two are
// solved separately and need not add up. The inter-method engine (SECTION B,
// added by a later commit) propagates counts to unprofiled methods (and bounds
// a caller by an exact profiled callee's `call_count`), but does NOT fully
// reconcile a caller's invoke count with a profiled callee's own `call_count`
// -- that gap persists even with the engine on.

namespace {

// Closed-form geometric loop amplification for a reducible natural-loop
// header: `1/(1-p_back)`, clamped to `[1, loop_iteration_cap]`. `p_back` is the
// summed back-edge probability into the header; a covered header with no
// measured back-edge falls back to `default_backedge_prob`. The `p_back` clamp
// to `cap_pb = 1 - 1/cap` keeps `1-p_back >= 1/cap > 0`, so the reciprocal is
// never a divide-by-~0 (no inf/negative multiplier). Shared by the forward
// solve and the re-flow allocation.
double loop_geometric_mult(double p_back,
                           bool covered_hot,
                           float loop_iteration_cap,
                           float default_backedge_prob) {
  if (p_back <= 0.0 && covered_hot) {
    p_back = default_backedge_prob;
  }
  const double cap = (double)loop_iteration_cap;
  const double cap_pb = 1.0 - 1.0 / cap;
  if (p_back > cap_pb) {
    p_back = cap_pb;
  }
  if (p_back >= 1.0) {
    return cap;
  }
  return std::clamp(1.0 / (1.0 - p_back), 1.0, cap);
}

// (2) Per-out-edge probability estimation for interaction slot `i`. Each edge's
// weight combines a structural backbone (uniform split over forward
// successors), the target's appear100 prior, a coverage pin (cold targets get
// no flow), and exception-handler coldness on throw edges.
// Shape-only (independent of the entry anchor), so it can be computed once and
// reused by both the forward solve and the re-flow allocation.
UnorderedMap<const cfg::Edge*, double> compute_edge_prob(
    cfg::ControlFlowGraph& cfg,
    size_t i,
    float handler_cold_factor,
    size_t* zero_outflow_sinks) {
  UnorderedMap<const cfg::Edge*, double> edge_prob;
  for (auto* b : cfg.blocks_view()) {
    const auto& succs = b->succs();
    if (succs.empty()) {
      continue;
    }
    // Number of ordinary forward successors (GOTO/BRANCH); throw and ghost
    // edges are handled separately below and excluded from the uniform split.
    size_t n_flow = 0;
    for (auto* e : succs) {
      const auto t = e->type();
      if (t == cfg::EDGE_GOTO || t == cfg::EDGE_BRANCH) {
        n_flow++;
      }
    }

    double sum = 0.0;
    std::vector<std::pair<cfg::Edge*, double>> ws;
    ws.reserve(succs.size());
    for (auto* e : succs) {
      const auto t = e->type();
      double w = 0.0;
      auto* tsb = source_blocks::get_first_source_block(e->target());
      const auto tval = (tsb != nullptr) ? tsb->get_val(i) : std::nullopt;
      if (t == cfg::EDGE_GHOST) {
        w = 0.0; // fake exit edge
      } else if (t == cfg::EDGE_THROW) {
        w = handler_cold_factor; // throw edge: fixed exception-handler coldness
      } else {
        // Structural backbone: every forward successor gets an equal
        // share; a lone successor gets 1.0 (== 1.0 / n_flow at n_flow == 1).
        // Branch direction is an invertible encoding artifact, so we never bias
        // one arm over another.
        w = 1.0 / (double)n_flow;
        // Scale by the target's appear100 prior.
        const auto tappear =
            (tsb != nullptr) ? tsb->get_appear100(i) : std::nullopt;
        if (tappear && *tappear > 0.0f) {
          w *= (double)*tappear;
        }
      }
      // Coverage pin: a target that is cold for THIS interaction
      // receives no flow.
      if (t != cfg::EDGE_GHOST && tval && *tval == 0.0f) {
        w = 0.0;
      }
      ws.emplace_back(e, w);
      sum += w;
    }
    if (sum > 0.0) {
      for (auto& [e, w] : ws) {
        if (w > 0.0) {
          edge_prob[e] = w / sum;
        }
      }
    } else {
      (*zero_outflow_sinks)++;
    }
  }
  return edge_prob;
}

// (3) Forward block-frequency solve over reverse-postorder, given precomputed
// `edge_prob`. Result is LINEAR in `entry_anchor`. Reducible loop headers get
// the closed-form geometric multiplier; irreducible headers are treated as
// ordinary merges (and counted).
UnorderedMap<const cfg::Block*, double> forward_propagate(
    size_t i,
    double entry_anchor,
    loop_impl::LoopInfo& loops,
    const std::vector<cfg::Block*>& rpo,
    const UnorderedMap<const cfg::Block*, size_t>& rpo_index,
    cfg::Block* entry,
    const UnorderedMap<const cfg::Edge*, double>& edge_prob,
    float loop_iteration_cap,
    float default_backedge_prob,
    size_t* irreducible) {
  const size_t entry_idx = rpo_index.at(entry);

  // Loops innermost-first (deepest depth first) so an inner loop's multiplier
  // is known before an enclosing loop's local propagation crosses it.
  std::vector<loop_impl::Loop*> loops_inner_first;
  for (auto& loop : loops) {
    loops_inner_first.push_back(&loop);
  }
  std::stable_sort(loops_inner_first.begin(), loops_inner_first.end(),
                   [](loop_impl::Loop* a, loop_impl::Loop* b) {
                     return a->get_loop_depth() > b->get_loop_depth();
                   });

  // (2.5) Per-loop cyclic multiplier. p_back is the header-anchored probability
  // of taking a back-edge:
  //   p_back = Σ_latch freq_local(latch) · edge_prob(latch→header),
  // where freq_local is the intra-loop frequency with the header pinned to 1
  // (inner loops collapsed via their already-computed multiplier). Summing the
  // RAW back-edge probability alone (ignoring freq_local) over-counts any loop
  // whose exit is at the header (e.g. `while (cond)`), which is the common
  // case.
  UnorderedMap<const cfg::Block*, double> loop_mult; // header -> multiplier
  for (loop_impl::Loop* loop : loops_inner_first) {
    auto* header = loop->get_header();
    auto header_it = rpo_index.find(header);
    if (header_it == rpo_index.end()) {
      // Loop unreachable from entry: it never contributes to the forward solve
      // (the forward pass iterates rpo, which excludes it). This pass is a pure
      // annotator and does not prune unreachable blocks, so such loops can be
      // present; skip cheaply.
      loop_mult[header] = 1.0;
      continue;
    }
    const size_t header_idx = header_it->second;
    // Reducible iff the loop is single-entry: no block other than the header
    // has a predecessor from outside the loop. This is the natural-loop
    // definition, equivalent to header-dominance of every back-edge source but
    // requiring neither a dominator tree nor the full-reachability
    // (CFG-mutating) prep it would need -- so the pass stays a pure annotator.
    bool reducible = true;
    for (auto* b : loop->get_blocks()) {
      if (b == header) {
        continue;
      }
      for (auto* pe : b->preds()) {
        if (!loop->contains(pe->src())) {
          reducible = false;
          break;
        }
      }
      if (!reducible) {
        break;
      }
    }
    if (!reducible) {
      // Irreducible / multi-entry SCC: treat the header as an ordinary merge.
      (*irreducible)++;
      loop_mult[header] = 1.0;
      continue;
    }
    // Local header-anchored acyclic propagation over ONLY the loop's own blocks
    // in RPO order. Iterating the loop body (not the whole method) keeps total
    // work at O(Σ_loops |loop|·log|loop|) rather than O(#loops · #blocks) --
    // the latter is quadratic on methods with many loops and was the dominant
    // cost.
    std::vector<cfg::Block*> loop_blocks;
    for (auto* b : loop->get_blocks()) {
      loop_blocks.push_back(b);
    }
    std::sort(loop_blocks.begin(), loop_blocks.end(),
              [&](cfg::Block* a, cfg::Block* c) {
                return rpo_index.at(a) < rpo_index.at(c);
              });
    UnorderedMap<const cfg::Block*, double> fl;
    fl[header] = 1.0;
    for (auto* b : loop_blocks) {
      if (b == header) {
        continue;
      }
      const size_t b_idx = rpo_index.at(b);
      double in = 0.0;
      for (auto* pe : b->preds()) {
        auto* u = pe->src();
        if (!loop->contains(u)) {
          continue; // only intra-loop forward edges build freq_local
        }
        auto uit = rpo_index.find(u);
        const size_t u_idx = uit != rpo_index.end() ? uit->second : entry_idx;
        if (u_idx >= b_idx) {
          continue; // back-edge (into an inner header): carried by its mult
        }
        auto epit = edge_prob.find(pe);
        const double ep = epit != edge_prob.end() ? epit->second : 0.0;
        const double uf = (fl.count(u) != 0u) ? fl[u] : 0.0;
        in += uf * ep;
      }
      auto lmit = loop_mult.find(b);
      if (lmit != loop_mult.end()) {
        in *= lmit->second; // b is an inner loop header
      }
      fl[b] = in;
    }
    // p_back over back-edges into the header (source inside the loop).
    double p_back = 0.0;
    for (auto* pe : header->preds()) {
      auto* u = pe->src();
      if (!loop->contains(u)) {
        continue;
      }
      auto uit = rpo_index.find(u);
      const size_t u_idx = uit != rpo_index.end() ? uit->second : entry_idx;
      if (u_idx < header_idx) {
        continue; // forward pred inside the loop (not a back-edge)
      }
      auto epit = edge_prob.find(pe);
      const double ep = epit != edge_prob.end() ? epit->second : 0.0;
      const double uf = (fl.count(u) != 0u) ? fl[u] : 0.0;
      p_back += uf * ep;
    }
    auto* hsb = source_blocks::get_first_source_block(header);
    const auto hval = (hsb != nullptr) ? hsb->get_val(i) : std::nullopt;
    const bool covered_hot = hval && *hval > 0.0f;
    loop_mult[header] = loop_geometric_mult(
        p_back, covered_hot, loop_iteration_cap, default_backedge_prob);
  }

  // (3) Forward block-frequency solve over reverse-postorder, applying the
  // precomputed loop multiplier at each header.
  UnorderedMap<const cfg::Block*, double> block_freq;
  block_freq[entry] = entry_anchor;
  for (auto* b : rpo) {
    if (b == entry) {
      continue;
    }
    const size_t b_idx = rpo_index.at(b);
    double inflow = 0.0;
    for (auto* e : b->preds()) {
      auto* u = e->src();
      auto it = rpo_index.find(u);
      const size_t u_idx = it != rpo_index.end() ? it->second : entry_idx;
      if (u_idx >= b_idx) {
        continue; // back-edge: not part of forward inflow
      }
      auto epit = edge_prob.find(e);
      const double ep = (epit != edge_prob.end()) ? epit->second : 0.0;
      const double src_freq = (block_freq.count(u) != 0u) ? block_freq[u] : 0.0;
      inflow += src_freq * ep;
    }
    auto lmit = loop_mult.find(b);
    block_freq[b] = (lmit != loop_mult.end()) ? inflow * lmit->second : inflow;
  }
  return block_freq;
}

// Wu-Larus reverse-post-order block-frequency solve for interaction slot `i`,
// with the entry block anchored to `entry_anchor` (steps 2 and 3 of the
// per-method solve). The result is LINEAR in `entry_anchor`: passing 1.0 yields
// a per-method "shape" (relative per-block frequency) that a caller can cache
// once and scale by any entry count -- this is what the inter-method engine
// consumes as shape_M(b) / k_C(invoke). `epsilon` flooring is intentionally NOT
// applied here (it belongs at the write site).
UnorderedMap<const cfg::Block*, double> solve_block_freq(
    cfg::ControlFlowGraph& cfg,
    size_t i,
    double entry_anchor,
    loop_impl::LoopInfo& loops,
    const std::vector<cfg::Block*>& rpo,
    const UnorderedMap<const cfg::Block*, size_t>& rpo_index,
    cfg::Block* entry,
    float handler_cold_factor,
    float loop_iteration_cap,
    float default_backedge_prob,
    size_t* zero_outflow_sinks,
    size_t* irreducible) {
  const auto edge_prob =
      compute_edge_prob(cfg, i, handler_cold_factor, zero_outflow_sinks);
  return forward_propagate(i, entry_anchor, loops, rpo, rpo_index, entry,
                           edge_prob, loop_iteration_cap, default_backedge_prob,
                           irreducible);
}

// Reverse-postorder of `cfg` plus a block -> rpo-position index. Shared solve
// scaffolding for the per-method frequency solves.
void build_rpo(cfg::ControlFlowGraph& cfg,
               std::vector<cfg::Block*>& rpo,
               UnorderedMap<const cfg::Block*, size_t>& rpo_index) {
  rpo = graph::postorder_sort<cfg::GraphInterface>(cfg);
  std::reverse(rpo.begin(), rpo.end());
  rpo_index.reserve(rpo.size());
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    rpo_index[rpo[idx]] = idx;
  }
}

} // namespace

std::string SyntheticBlockCountsPass::get_config_doc() {
  return trim(R"(
Simulation/testing pass that rewrites boolean-ish SourceBlock `val` magnitudes
into flow-consistent synthetic per-block counts via a per-method,
entry-anchored Wu-Larus reverse-post-order solve (entry anchored to the
method's `call_count`, branch probabilities from coverage + loop/exception
heuristics). It pins the coverage support -- it never turns a 0 into >0 or a
covered block into 0 -- and never touches `appear100`, so enabling it is
No-Functional-Change for every existing optimization decision (all of which
test only `val > 0`). It is a pure SourceBlock annotator -- it never edits the
CFG (reducibility is decided by a single-entry loop test, so no dominator solve
and no dead-block removal are needed), so it is safe over `no_optimizations`
classes and leaves unprofiled methods byte-identical. It is gated off via a
`.disabled(True)` pass-list entry.
  )");
}

void SyntheticBlockCountsPass::bind_config() {
  bind("handler_cold_factor",
       m_handler_cold_factor,
       m_handler_cold_factor,
       "Probability mass placed on EDGE_THROW edges.");
  bind("loop_iteration_cap",
       m_loop_iteration_cap,
       m_loop_iteration_cap,
       "Maximum geometric loop multiplier per loop level.");
  bind("default_backedge_prob",
       m_default_backedge_prob,
       m_default_backedge_prob,
       "Assumed back-edge probability when coverage doesn't pin it.");
  bind("epsilon",
       m_epsilon,
       m_epsilon,
       "Floor for a covered block whose solved frequency underflowed; keeps "
       "covered blocks strictly > 0.");

  // Validate config values HERE, never in the bind_config() body above:
  // bind_config() is also run for config reflection (doc generation), where the
  // fields still hold their defaults and an assert would spuriously abort. This
  // callback runs only when a real config is consumed, after every bind().
  after_configuration([this] {
    // `loop_iteration_cap` is an upper bound on a multiplier, so it must be
    // >= 1.0; below 1.0 it becomes the `hi` of `std::clamp(mult, 1.0, cap)`
    // with lo > hi in the loop-header solve, which is undefined behavior.
    always_assert_log(m_loop_iteration_cap >= 1.0f,
                      "loop_iteration_cap must be >= 1.0, got %f",
                      m_loop_iteration_cap);
    // The floor exists only to keep a covered block strictly positive, so the
    // bound is representability, not any consumer's threshold. See
    // source_blocks::kMinPositiveCount for why the old
    // "must dominate min_block_hits" sizing no longer applies.
    always_assert_log(m_epsilon >= source_blocks::kMinPositiveCount,
                      "epsilon (%f) must be at least the smallest normal float "
                      "to keep covered blocks strictly > 0",
                      m_epsilon);
    // `default_backedge_prob` is used as the fallback back-edge probability
    // p_back; outside [0, 1] it makes `1-p_back` a non-positive divisor (or
    // yields >1 flow). Validate up front rather than clamping silently.
    always_assert_log(m_default_backedge_prob >= 0.0f &&
                          m_default_backedge_prob <= 1.0f,
                      "default_backedge_prob (%f) must be in [0, 1]",
                      m_default_backedge_prob);
    // Handler weight scales exception-edge flow; a non-finite or negative
    // factor would poison every downstream count with NaN/negative flow.
    always_assert_log(std::isfinite(m_handler_cold_factor) &&
                          m_handler_cold_factor >= 0.0f,
                      "handler_cold_factor (%f) must be finite and >= 0",
                      m_handler_cold_factor);
  });
}

SyntheticBlockCountsPass::MethodResult SyntheticBlockCountsPass::process_method(
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const method_profiles::MethodProfiles& profiles,
    const std::vector<std::string>& inv_slot) {
  MethodResult res;
  res.methods_seen = 1;

  // Bind a const reference to select the read-only LoopInfo ctor; the
  // non-const ctor mutates the CFG by inserting preheaders.
  const cfg::ControlFlowGraph& ccfg = cfg;
  loop_impl::LoopInfo loops(ccfg);
  std::vector<cfg::Block*> rpo;
  UnorderedMap<const cfg::Block*, size_t> rpo_index;
  build_rpo(cfg, rpo, rpo_index);
  auto* entry = cfg.entry_block();

  {
    auto* eloop = loops.get_loop_for(entry);
    if (eloop != nullptr && eloop->get_header() == entry) {
      res.solve_fallback_entry_is_header++;
    }
  }

  // (method, interaction) sparsity: count covered vs uncovered
  // pairs (covered = >=1 val>0 block for that interaction). Uncovered pairs
  // write nothing (support pinned) and are the skippable set. One scan over all
  // interactions.
  {
    std::vector<bool> covered(inv_slot.size(), false);
    for (auto* b : cfg.blocks_view()) {
      source_blocks::foreach_source_block(b, [&](SourceBlock* sb) {
        for (size_t i = 0; i < inv_slot.size(); ++i) {
          auto v = sb->get_val(i);
          if (v && *v > 0.0f) {
            covered.at(i) = true;
          }
        }
      });
    }
    for (size_t i = 0; i < inv_slot.size(); ++i) {
      res.pairs_total++;
      if (covered.at(i)) {
        res.pairs_covered++;
      } else {
        res.pairs_uncovered++;
      }
    }
  }

  bool any_slot_used = false;

  for (size_t i = 0; i < inv_slot.size(); ++i) {
    auto stat = profiles.get_method_stat(inv_slot[i], method);
    if (!stat) {
      // No numeric seed for this interaction -- leave the boolean vals as-is.
      res.solve_fallback_no_profile++;
      continue;
    }
    const double call_count = stat->call_count;
    if (!std::isfinite(call_count) || call_count < 0.0) {
      // A non-finite (NaN/inf) or negative call_count is not a usable anchor --
      // treat it like no profile (leave the boolean vals as-is for this
      // interaction), rather than propagating NaN/inf into every block or
      // collapsing every covered block to epsilon from a garbage negative
      // anchor. (call_count == 0 is usable: it correctly floors to epsilon.)
      res.solve_fallback_no_profile++;
      continue;
    }
    any_slot_used = true;

    // Anchor the entry to `call_count`; because the solve is linear in the
    // anchor this is identical to scaling a unit-anchored shape by call_count.
    auto block_freq = solve_block_freq(cfg,
                                       i,
                                       /*entry_anchor=*/call_count,
                                       loops,
                                       rpo,
                                       rpo_index,
                                       entry,
                                       m_handler_cold_factor,
                                       m_loop_iteration_cap,
                                       m_default_backedge_prob,
                                       &res.zero_outflow_sinks,
                                       &res.solve_fallback_irreducible);

    // (4) Write magnitude only; the write guard is the sole enforcer of
    // support preservation.
    for (auto* b : cfg.blocks_view()) {
      auto freq_it = block_freq.find(b);
      if (freq_it == block_freq.end()) {
        // The block is unreachable from entry in the solve (this pass is a pure
        // annotator and does not prune unreachable blocks, so a
        // covered-but-unreachable block can exist). We have no frequency
        // estimate for it, so leave its existing
        // val untouched rather than flooring a genuine covered count to
        // epsilon.
        continue;
      }
      const double f = freq_it->second;
      source_blocks::foreach_source_block(b, [&](SourceBlock* sb) {
        sb->apply_at(i, [&](SourceBlock::Val& v) {
          if (!v || !(v->val > 0.0f)) {
            // preserve 0 / NaN; never resurrect the support. Written as
            // `!(val > 0)` rather than `val <= 0` so a NaN val (for which every
            // ordered compare is false) also takes this skip path.
            return;
          }
          float m = (float)f;
          if (m < m_epsilon) {
            m = m_epsilon; // keep covered blocks strictly > 0
            res.epsilon_clamped_blocks++;
          }
          // Only `val` is written, only for already-covered blocks (guarded
          // above), and only to a strictly-positive m >= epsilon -- so the
          // coverage support and appear100 are preserved by construction. A
          // post-write "did support change?" check here is vacuous (it can
          // never fire), so it is intentionally omitted.
          v->val = m;
          res.blocks_written++;
        });
      });
    }
  }

  if (any_slot_used) {
    res.methods_with_usable_profile = 1;
  }
  return res;
}

// Per interaction, count methods that ARE covered (>=1 SourceBlock val > 0) yet
// have NO usable call_count anchor -- no profile row, or a non-finite /
// negative call_count (a call_count of 0 counts as a usable anchor, per
// `usable_call_count`). This is the covered-but-unprofiled population the
// inter-method forward-fill exists to heat -- often produced by inlining a hot
// callee into a method with no profile of its own. Pure: no pass state, writes
// nothing; run_pass emits the values as metrics.
std::vector<int64_t> SyntheticBlockCountsPass::count_missing_hit_methods(
    const Scope& scope,
    const method_profiles::MethodProfiles& profiles,
    const std::vector<std::string>& inv_slot) {
  const size_t n = inv_slot.size();
  // Array (not vector) of atomics: std::vector<std::atomic<>> is ill-formed
  // (atomics are not movable). make_unique<T[]> value-initializes to 0.
  auto counts = std::make_unique<std::atomic<size_t>[]>(n);
  walk::parallel::methods(scope, [&](DexMethod* m) {
    auto* code = m->get_code();
    if (code == nullptr || !code->cfg_built()) {
      return;
    }
    auto& cfg = code->cfg();
    // A (method, interaction) is "covered" if any block's representative
    // SourceBlock has val > 0 for that interaction (the coverage notion the
    // solve uses). One scan over blocks fills every interaction.
    std::vector<bool> covered(n, false);
    for (auto* b : cfg.blocks()) {
      // Covered if ANY source block in the block ran for this interaction --
      // the same `foreach_source_block` predicate the write-back uses, so this
      // metric measures the exact population the solve writes to (a block may
      // carry several source blocks after inlining, not just a representative).
      source_blocks::foreach_source_block(b, [&](SourceBlock* sb) {
        for (size_t i = 0; i < n; ++i) {
          if (covered[i]) {
            continue;
          }
          const auto v = sb->get_val(i);
          if (v && *v > 0.0f) {
            covered[i] = true;
          }
        }
      });
    }
    for (size_t i = 0; i < n; ++i) {
      if (!covered[i]) {
        continue;
      }
      auto stat = profiles.get_method_stat(inv_slot[i], m);
      // Covered but no usable anchor: absent row, or a non-finite / negative
      // call_count. A call_count of exactly 0 IS usable (a genuinely cold but
      // profiled method, which the solve floors to epsilon), matching
      // `usable_call_count` -- so it is a real anchor, not a "missing hit".
      if (!stat || !std::isfinite(stat->call_count) || stat->call_count < 0.0) {
        counts[i].fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  std::vector<int64_t> out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = (int64_t)counts[i].load(std::memory_order_relaxed);
  }
  return out;
}

void SyntheticBlockCountsPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  const auto& profiles = conf.get_method_profiles();
  const auto& slot_map = g_redex->get_sb_interaction_indices();
  const size_t n = g_redex->num_sb_interaction_indices();
  std::vector<std::string> inv_slot(n);
  for (const auto& kv : UnorderedIterable(slot_map)) {
    always_assert(kv.second < n);
    inv_slot[kv.second] = kv.first;
  }

  auto scope = build_class_scope(stores);

  // Per-method producer wall-clock, emitted as a `*_ms` metric into the redex
  // stats so the pass cost stays observable.
  using clk = std::chrono::steady_clock;

  // Emitted before the producer so it lands on the
  // per-method path (and on the inter-method path once that engine lands later
  // in the stack). Metric-only, NFC.
  {
    const auto missing_hit =
        count_missing_hit_methods(scope, profiles, inv_slot);
    int64_t total = 0;
    for (size_t i = 0; i < inv_slot.size(); ++i) {
      mgr.set_metric("missing_hit_methods_" + inv_slot[i], missing_hit[i]);
      total += missing_hit[i];
    }
    mgr.set_metric("missing_hit_methods_total", total);
  }

  const auto t0 = clk::now();
  auto res = walk::parallel::methods<MethodResult>(
      scope, [&](DexMethod* m) -> MethodResult {
        auto* code = m->get_code();
        if (code == nullptr || !code->cfg_built()) {
          return MethodResult{};
        }
        return process_method(m, code->cfg(), profiles, inv_slot);
      });

  mgr.set_metric("methods_seen", (int64_t)res.methods_seen);
  mgr.set_metric("methods_with_usable_profile",
                 (int64_t)res.methods_with_usable_profile);
  mgr.set_metric("blocks_written", (int64_t)res.blocks_written);
  mgr.set_metric("solve_fallback_no_profile",
                 (int64_t)res.solve_fallback_no_profile);
  mgr.set_metric("solve_fallback_irreducible",
                 (int64_t)res.solve_fallback_irreducible);
  mgr.set_metric("solve_fallback_entry_is_header",
                 (int64_t)res.solve_fallback_entry_is_header);
  mgr.set_metric("epsilon_clamped_blocks", (int64_t)res.epsilon_clamped_blocks);
  mgr.set_metric("zero_outflow_sinks", (int64_t)res.zero_outflow_sinks);
  mgr.set_metric("pairs_total", (int64_t)res.pairs_total);
  mgr.set_metric("pairs_covered", (int64_t)res.pairs_covered);
  mgr.set_metric("pairs_uncovered", (int64_t)res.pairs_uncovered);
  mgr.set_metric(
      "permethod_solve_ms",
      (int64_t)std::chrono::duration<double, std::milli>(clk::now() - t0)
          .count());
}

static SyntheticBlockCountsPass s_pass;
