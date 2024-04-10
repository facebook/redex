/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexReshufflePass.h"
#include "ClassMerging.h"
#include "ConfigFiles.h"
#include "DedupStrings.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "ModelSpecGenerator.h"
#include "PassManager.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
const interdex::InterDexPass* get_interdex_pass(const PassManager& mgr) {
  const auto* pass =
      static_cast<interdex::InterDexPass*>(mgr.find_pass("InterDexPass"));
  always_assert_log(pass, "InterDexPass missing");
  return pass;
}
} // namespace

void InterDexReshufflePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  const auto* interdex_pass = get_interdex_pass(mgr);
  if (!interdex_pass->minimize_cross_dex_refs()) {
    mgr.incr_metric("no minimize_cross_dex_refs", 1);
    TRACE(
        IDEXR, 1,
        "InterDexReshufflePass not run because InterDexPass is not configured "
        "for minimize_cross_dex_refs.");
    return;
  }

  auto original_scope = build_class_scope(stores);

  auto& root_store = stores.at(0);
  auto& root_dexen = root_store.get_dexen();
  if (root_dexen.size() == 1) {
    // only a primary dex? Nothing to do
    return;
  }

  std::set<size_t> untouched_dexes;
  for (size_t dex_index = 1; dex_index < root_dexen.size(); dex_index++) {
    auto& dex = root_dexen.at(dex_index);
    bool should_be_touched = false;
    for (auto cls : dex) {
      if (is_canary(cls)) {
        continue;
      }
      if (!cls->is_dynamically_dead()) {
        should_be_touched = true;
        break;
      }
    }
    if (!should_be_touched) {
      untouched_dexes.emplace(dex_index);
      TRACE(IDEXR, 1, "**dex %zu should not be touched\n", dex_index);
    }
  }

  auto has_IDCM_pass = mgr.find_pass("IntraDexClassMergingPass");
  // The mergeability_aware reshuffle algorithm is only enabled when 1) there
  // will be a IDCM pass; 2) m_allow_mergeability_aware is set true; and 3) This
  // is the first run of InterDexReshufflePass.
  bool enable_mergeability_aware_reshuffle =
      has_IDCM_pass && m_allow_mergeability_aware && m_run == 0;
  if (enable_mergeability_aware_reshuffle) {
    TRACE(PM, 1, "Run regular mergeability-aware InterDexReshuffle");
    mgr.incr_metric("Mergeability_aware", 1);
    class_merging::Model merging_model = class_merging::construct_global_model(
        original_scope, mgr, conf, stores);

    InterDexReshuffleImpl impl(conf, mgr, m_config, original_scope, root_dexen,
                               untouched_dexes, merging_model);
    impl.compute_plan();
    impl.apply_plan();
  } else {
    TRACE(PM, 1, "Run regular InterDexReshuffle");
    mgr.incr_metric("Mergeability_aware", 0);
    InterDexReshuffleImpl impl(conf, mgr, m_config, original_scope, root_dexen,
                               untouched_dexes);
    impl.compute_plan();
    impl.apply_plan();
  }

  // Sanity check
  std::unordered_set<DexClass*> original_scope_set(original_scope.begin(),
                                                   original_scope.end());
  auto new_scope = build_class_scope(stores);
  std::unordered_set<DexClass*> new_scope_set(new_scope.begin(),
                                              new_scope.end());
  always_assert(original_scope_set.size() == new_scope_set.size());
  for (auto cls : original_scope_set) {
    always_assert(new_scope_set.count(cls));
  }

  ++m_run;
}

static InterDexReshufflePass s_pass;
