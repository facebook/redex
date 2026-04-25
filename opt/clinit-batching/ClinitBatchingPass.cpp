/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClinitBatchingPass.h"

#include <boost/regex.hpp> // NOLINT(facebook-unused-include-check)

#include "BaselineProfile.h"
#include "ConfigFiles.h"
#include "DeterministicContainers.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "MethodProfiles.h"
#include "MethodUtil.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

void ClinitBatchingPass::bind_config() {
  bind("interaction_pattern", "", m_interaction_pattern,
       "Regex pattern to filter baseline profile interactions (e.g., "
       "``ColdStart``). Only clinits hot in matching interactions are "
       "candidates for batching.");
  trait(Traits::Pass::unique, true);
}

InsertOnlyConcurrentMap<DexMethod*, DexClass*>
ClinitBatchingPass::identify_candidate_clinits(const Scope& scope,
                                               ConfigFiles& conf,
                                               PassManager& mgr) {
  // Load baseline profile config and filter by interaction pattern
  auto baseline_profile_config = conf.get_default_baseline_profile_config();
  if (!m_interaction_pattern.empty()) {
    boost::regex rx(m_interaction_pattern);
    unordered_erase_if(
        baseline_profile_config.interaction_configs,
        [&](auto& p) { return !boost::regex_match(p.first, rx); });
  }

  // Log interaction configs
  for (auto& [interaction_id, config] :
       UnorderedIterable(baseline_profile_config.interaction_configs)) {
    mgr.set_metric("interaction_" + interaction_id, config.threshold);
  }

  // Build baseline profile from method profiles using the filtered config
  baseline_profiles::BaselineProfileConfigMap bp_conf_map = {
      {baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME,
       std::move(baseline_profile_config)}};
  auto baseline_profile = baseline_profiles::get_default_baseline_profile(
      scope, bp_conf_map, conf.get_method_profiles());

  // Track statistics
  std::atomic<size_t> excluded_not_hot{0};
  std::atomic<size_t> excluded_no_optimizations{0};
  std::atomic<size_t> excluded_is_root{0};
  std::atomic<size_t> excluded_cannot_rename{0};
  std::atomic<size_t> excluded_is_enum{0};

  InsertOnlyConcurrentMap<DexMethod*, DexClass*> candidate_clinits;

  walk::parallel::classes(scope, [&](DexClass* cls) {
    DexMethod* method = cls->get_clinit();
    if (method == nullptr) {
      return;
    }

    // Skip methods marked as no_optimizations or should_not_outline
    if (method->rstate.no_optimizations() ||
        method->rstate.should_not_outline()) {
      excluded_no_optimizations.fetch_add(1);
      return;
    }

    // Check if clinit is hot in baseline profile
    auto it = baseline_profile.methods.find(method);
    if (it == baseline_profile.methods.end()) {
      excluded_not_hot.fetch_add(1);
      return;
    }
    if (!it->second.hot) {
      excluded_not_hot.fetch_add(1);
      return;
    }

    // Skip classes that are reachability roots (via ProGuard -keep,
    // @DoNotStrip, etc.). These classes may have semantic requirements on their
    // clinit behavior (e.g., JNI registration, security checks).
    if (!cls->rstate.can_delete()) {
      excluded_is_root.fetch_add(1);
      TRACE(CLINIT_BATCHING,
            4,
            "ClinitBatchingPass: excluding %s - class is a reachability root",
            SHOW(cls));
      return;
    }

    // Skip classes that cannot be renamed. If a class cannot be renamed, it's
    // likely being referenced externally (JNI, reflection, serialization) and
    // its clinit semantics may be important.
    if (!cls->rstate.can_rename()) {
      excluded_cannot_rename.fetch_add(1);
      TRACE(CLINIT_BATCHING,
            4,
            "ClinitBatchingPass: excluding %s - class cannot be renamed",
            SHOW(cls));
      return;
    }

    // Skip enum classes. Enums have a $VALUES static field that is initialized
    // in the clinit and accessed by the auto-generated values() method. The
    // runtime (via Class.getEnumConstants()) can call values() at any time
    // through reflection, before our batched initialization runs, causing NPE.
    //
    // TODO(jimmycleary): This enum exclusion was added by Claude to fix a
    // crash but the reasoning seems dubious — the concern about $VALUES being
    // accessed before batched init applies equally to all static fields, which
    // is what EarlyClassLoadsAnalysis handles. Verify whether enums actually
    // need special treatment before removing.
    if (is_enum(cls)) {
      excluded_is_enum.fetch_add(1);
      TRACE(CLINIT_BATCHING,
            4,
            "ClinitBatchingPass: excluding %s - class is an enum",
            SHOW(cls));
      return;
    }

    candidate_clinits.emplace(method, cls);
    auto* code = method->get_code();
    TRACE(CLINIT_BATCHING,
          3,
          "ClinitBatchingPass: candidate clinit %s (size=%u)",
          SHOW(method),
          code ? code->estimate_code_units() : 0);
  });

  // Report metrics
  mgr.set_metric("candidate_clinits_count", candidate_clinits.size());
  mgr.set_metric("excluded_not_hot", excluded_not_hot.load());
  mgr.set_metric("excluded_no_optimizations", excluded_no_optimizations.load());
  mgr.set_metric("excluded_is_root", excluded_is_root.load());
  mgr.set_metric("excluded_cannot_rename", excluded_cannot_rename.load());
  mgr.set_metric("excluded_is_enum", excluded_is_enum.load());

  TRACE(CLINIT_BATCHING,
        1,
        "ClinitBatchingPass: identified %zu candidate clinits "
        "(excluded: %zu not hot, %zu no_optimizations, "
        "%zu is_root, %zu cannot_rename, %zu is_enum)",
        candidate_clinits.size(),
        excluded_not_hot.load(),
        excluded_no_optimizations.load(),
        excluded_is_root.load(),
        excluded_cannot_rename.load(),
        excluded_is_enum.load());

  return candidate_clinits;
}

void ClinitBatchingPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  auto scope = build_class_scope(stores);

  // Step 1: Identify candidate clinits based on baseline profile
  auto candidate_clinits = identify_candidate_clinits(scope, conf, mgr);

  // Future steps (T3-T6):
  // - Build dependency graph between candidate clinits
  // - Transform clinits: extract body to __initStatics$*() methods
  // - Generate orchestrator method body
  // - Validate correctness

  mgr.set_metric("batched_clinits", candidate_clinits.size());
  mgr.set_metric("interaction_pattern_set",
                 m_interaction_pattern.empty() ? 0 : 1);
  TRACE(CLINIT_BATCHING,
        1,
        "ClinitBatchingPass: found %zu clinits for batching",
        candidate_clinits.size());
}

static ClinitBatchingPass s_pass;
