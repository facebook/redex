/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringSwitchTransform.h"

#include <algorithm>
#include <memory>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "LocalDce.h"
#include "StringSwitchFinder.h"

namespace {

namespace cp = constant_propagation;

std::shared_ptr<cp::intraprocedural::FixpointIterator> run_fixpoint(
    cfg::ControlFlowGraph& cfg) {
  auto fixpoint = std::make_shared<cp::intraprocedural::FixpointIterator>(
      cfg, StringSwitchFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());
  return fixpoint;
}

} // namespace

void run_string_switch_transforms(
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const std::vector<std::unique_ptr<StringSwitchTransform>>& transforms,
    const UnorderedSet<DexMethodRef*>& pure_methods,
    const init_classes::InitClassesWithSideEffects& init_classes,
    DriverStats* stats) {
  if (transforms.empty()) {
    return;
  }
  // Budget the total rewriting work, starting from the number of switches first
  // recovered. Each apply() decrements the budget by its return value: a
  // terminal rewrite returns 1, while a rewrite that intentionally leaves a
  // recoverable switch for a follow-up transform (e.g. HotCaseExtractTransform
  // peeling hot cases and leaving a cold HASH_SWITCH for
  // StringTreeMapTransform) returns 0. Every terminal rewrite makes one
  // recovered switch unrecoverable, so at most the initial switch count of them
  // run; a 0-returning transform must leave the switch in a form it no longer
  // matches, so it cannot be re-selected forever. Together this bounds the loop
  // even if a transform is buggy.
  std::optional<size_t> budget;
  bool any_applied = false;
  while (true) {
    // Once the budget is exhausted there are no recoverable switches left (the
    // budget tracks their count), so stop here -- before rebuilding the
    // (expensive) fixpoint + reaching-def context and re-recovering. On the
    // first iteration the budget is not yet known and is seeded below.
    if (budget && *budget == 0) {
      break;
    }
    StringSwitchCfgContext ctx(cfg, run_fixpoint(cfg));
    auto switches = find_string_switches(ctx);
    if (!budget) {
      budget = switches.size();
    }

    // Across all switches and transforms, pick the single best applicable
    // (transform, switch) pair. Ties resolve to earlier switches, then earlier
    // registered transforms.
    const StringSwitchTransform* best_transform = nullptr;
    const StringSwitchInfo* best_info = nullptr;
    std::optional<TransformScore> best_score;
    for (const auto& info : switches) {
      StringSwitchCandidate candidate{method, ctx, info};
      for (const auto& transform : transforms) {
        auto score = transform->evaluate(candidate);
        if (score && (!best_score || is_better(*score, *best_score))) {
          best_score = std::move(score);
          best_transform = transform.get();
          best_info = &info;
        }
      }
    }
    if (best_transform == nullptr) {
      break;
    }
    always_assert(best_info != nullptr);
    StringSwitchCandidate winner{method, ctx, *best_info};
    size_t consumed = best_transform->apply(winner, best_score->plan.get());
    stats->record(best_transform->name());
    any_applied = true;
    // Clamp so a transform returning more than the remaining budget cannot
    // underflow it.
    *budget -= std::min(consumed, *budget);
    // The CFG is now mutated; loop to rebuild the context and re-recover. Each
    // apply leaves its now-dead dispatch machinery (the original
    // String.hashCode/equals invokes and the constants that fed them) in place;
    // find_string_switches skips those leftovers, so a single LocalDce after
    // the loop cleans up every transform's residue at once.
  }

  if (any_applied) {
    LocalDce(&init_classes, pure_methods).dce(cfg);
  }
}
