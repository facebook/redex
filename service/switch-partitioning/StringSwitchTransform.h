/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "StringSwitchFinder.h"

namespace init_classes {
class InitClassesWithSideEffects;
} // namespace init_classes

/*
 * Infrastructure for rewriting recovered string switches (StringSwitchInfo)
 * into alternate forms. Many transforms may apply to a given switch; each one
 * scores itself against a candidate, and the driver applies the single
 * best-scoring applicable transform, re-recovers the (now mutated) switches,
 * and repeats.
 */

// Transforms fall into priority tiers. A PERFORMANCE transform is always chosen
// over a SIZE transform, regardless of magnitude.
enum class TransformTier {
  SIZE = 0,
  PERFORMANCE = 1,
};

// Transform-specific data computed by evaluate() and reused by apply(), so the
// winning transform need not redo the (often expensive, e.g. trie-encoding)
// analysis. Each transform defines its own subclass and downcasts in apply();
// the driver only ever carries the base pointer inside the winning
// TransformScore. A plan may hold cfg::Block* pointers into the candidate's
// CFG: it is valid only until that CFG is mutated, which the driver guarantees
// does not happen between the evaluate() that produced it and the matching
// apply().
struct TransformPlan {
  TransformPlan() = default;
  virtual ~TransformPlan() = default;

  // Plans are only ever moved (into a std::optional, then a std::unique_ptr),
  // never copied. Deleting copy removes the deprecated implicit copy the
  // virtual destructor would otherwise generate and prevents slicing this
  // polymorphic base; defaulting move keeps derived plans movable (their
  // implicit move must move-construct this base subobject rather than fall back
  // to copying it).
  TransformPlan(const TransformPlan&) = delete;
  TransformPlan& operator=(const TransformPlan&) = delete;
  TransformPlan(TransformPlan&&) = default;
  TransformPlan& operator=(TransformPlan&&) = default;
};

struct TransformScore {
  TransformTier tier;
  // Within-tier ranking; may be a fixed engineer-assigned value or a derived
  // heuristic (e.g. estimated dex bytes saved). Higher is better.
  int64_t magnitude;
  // Precomputed data apply() consumes; the driver hands it to the winning
  // transform's apply(). Null for a transform whose apply() needs nothing
  // beyond the candidate.
  std::unique_ptr<TransformPlan> plan;

  // evaluate() returns a score with the plan its apply() will consume.
  TransformScore(TransformTier tier,
                 int64_t magnitude,
                 std::unique_ptr<TransformPlan> plan)
      : tier(tier), magnitude(magnitude), plan(std::move(plan)) {}

  // Overload for a transform that needs no plan: `plan` default-constructs to
  // null. (Declaring these constructors also makes TransformScore a non-
  // aggregate, so `TransformScore{tier, magnitude}` is a constructor call
  // rather than aggregate init -- no missing-field-initializer for `plan`.)
  TransformScore(TransformTier tier, int64_t magnitude)
      : tier(tier), magnitude(magnitude) {}
};

// Strict "is `a` a better choice than `b`?": higher tier wins; ties broken by
// magnitude.
inline bool is_better(const TransformScore& a, const TransformScore& b) {
  if (a.tier != b.tier) {
    return a.tier > b.tier;
  }
  return a.magnitude > b.magnitude;
}

// A per-transform request for reserved dex refs (summed by the pass in
// eval_pass and multiplied by the per-dex transform cap).
struct RefBudget {
  size_t frefs{0};
  size_t mrefs{0};
  size_t trefs{0};
};

// Everything a transform sees for one recovered switch. All references are
// valid only for the duration of an evaluate()/apply() call -- the driver
// rebuilds the context and re-recovers the switches between rounds.
struct StringSwitchCandidate {
  DexMethod* method;
  const StringSwitchCfgContext& ctx; // ctx.cfg(), ctx.fixpoint(), use/def
                                     // chains
  const StringSwitchInfo& info;
};

/*
 * A single way to rewrite a string switch. Implementations must be stateless
 * (holding only immutable configuration), since one instance is shared across
 * the parallel method walk: evaluate()/apply() operate solely on the
 * per-candidate CFG.
 */
class StringSwitchTransform {
 public:
  virtual ~StringSwitchTransform() = default;

  virtual std::string_view name() const = 0;

  // Read-only. Returns std::nullopt when this transform does not apply to the
  // candidate; otherwise a score the driver compares against other applicable
  // transforms. Must not mutate the CFG.
  virtual std::optional<TransformScore> evaluate(
      const StringSwitchCandidate& candidate) const = 0;

  // Performs the rewrite. Only ever called on the winning transform for a
  // candidate; mutates candidate.ctx.cfg(). `plan` is the TransformScore::plan
  // this transform's own evaluate() returned for this candidate (null if it
  // returned none) -- apply() downcasts it to its concrete plan type to reuse
  // that analysis instead of recomputing it.
  //
  // Returns how much to decrement the driver's budget (see
  // run_string_switch_transforms) for this application: 1 for a "terminal"
  // rewrite that leaves nothing further for the driver to do with this switch,
  // or 0 to signal that the rewrite deliberately left a recoverable switch
  // behind for a follow-up transform (e.g. HotCaseExtractTransform leaves a
  // cold HASH_SWITCH for StringTreeMapTransform). A transform that returns 0
  // MUST guarantee it will not itself be re-selected for the switch it just
  // rewrote (otherwise the driver could loop); it does so by rewriting the
  // switch into a form its own evaluate() no longer matches.
  virtual size_t apply(const StringSwitchCandidate& candidate,
                       const TransformPlan* plan) const = 0;

  // The dex refs a single application of this transform may introduce.
  virtual RefBudget reserve_refs() const { return {}; }
};

// Per-method/per-pass aggregate of what the driver did, keyed by transform
// name.
struct DriverStats {
  std::map<std::string, size_t> applied;

  void record(std::string_view transform_name) {
    applied[std::string(transform_name)]++;
  }
};

struct CombineDriverStats {
  void operator()(const DriverStats& addend, DriverStats* accumulator) const {
    for (const auto& [name, count] : addend.applied) {
      accumulator->applied[name] += count;
    }
  }
};

/*
 * Recovers the string switches in `cfg` and, for each, applies the best-scoring
 * applicable transform from `transforms` (priority by TransformScore). After an
 * application the CFG is mutated, so the driver re-recovers the switches and
 * repeats until no transform applies. `transforms` is shared read-only; it may
 * be empty (then this is a no-op). Updates `stats`.
 *
 * A budget, initialized to the number of switches first recovered, bounds the
 * work: each apply() decrements it by that apply()'s return value (1 for a
 * terminal rewrite, 0 for one that leaves a recoverable switch for a follow-up
 * transform). The loop stops when the budget is exhausted or no transform
 * applies. Because every terminal rewrite makes one recovered switch
 * unrecoverable, at most `switches.size()` of them ever run, so the budget is
 * both sufficient and a guard against a buggy transform re-applying forever.
 *
 * If any transform was applied, the driver runs LocalDce once at the end to
 * clear the dispatch machinery the rewrites left behind (the dead
 * String.hashCode/equals invokes and the constants that fed them).
 * `pure_methods` and `init_classes` are the side-effect-free set and init-class
 * info that DCE needs; both are method-independent, so the caller builds them
 * once and shares them across all methods.
 */
void run_string_switch_transforms(
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const std::vector<std::unique_ptr<StringSwitchTransform>>& transforms,
    const UnorderedSet<DexMethodRef*>& pure_methods,
    const init_classes::InitClassesWithSideEffects& init_classes,
    DriverStats* stats);
