/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringSwitchTransform.h"

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
  // Each successful transform should make at least one switch unrecoverable, so
  // the number of applications is bounded by the initial switch count. This cap
  // guarantees termination even if a transform fails to do so (a transform
  // bug): we never re-recover and re-apply forever.
  std::optional<size_t> budget;
  bool any_applied = false;
  while (true) {
    StringSwitchCfgContext ctx(cfg, run_fixpoint(cfg));
    auto switches = find_string_switches(ctx);
    if (!budget) {
      budget = switches.size();
    }
    if (*budget == 0) {
      break;
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
          best_score = score;
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
    best_transform->apply(winner);
    stats->record(best_transform->name());
    any_applied = true;
    --*budget;
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
