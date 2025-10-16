/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexReshufflePass.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "PassManager.h"
#include "Trace.h"

namespace {
const interdex::InterDexPass* get_interdex_pass(const PassManager& mgr) {
  const auto* pass =
      dynamic_cast<interdex::InterDexPass*>(mgr.find_pass("InterDexPass"));
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

  InterDexReshuffleImpl impl(conf, mgr, m_config, original_scope, root_dexen,
                             interdex_pass->get_dynamically_dead_dexes());
  impl.compute_plan();
  impl.apply_plan();

  // Sanity check
  UnorderedSet<DexClass*> original_scope_set(original_scope.begin(),
                                             original_scope.end());
  auto new_scope = build_class_scope(stores);
  UnorderedSet<DexClass*> new_scope_set(new_scope.begin(), new_scope.end());
  always_assert(original_scope_set.size() == new_scope_set.size());
  for (auto* cls : UnorderedIterable(original_scope_set)) {
    always_assert(new_scope_set.count(cls));
  }
}

static InterDexReshufflePass s_pass;
