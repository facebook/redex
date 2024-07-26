/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexRemovalPass.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "InterDexReshuffleImpl.h"
#include "PassManager.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "WorkQueue.h"

namespace {
const interdex::InterDexPass* get_interdex_pass(const PassManager& mgr) {
  const auto* pass =
      static_cast<interdex::InterDexPass*>(mgr.find_pass("InterDexPass"));
  always_assert_log(pass, "InterDexPass missing");
  return pass;
}
} // namespace

size_t DexRemovalPass::remove_empty_dexes(DexClassesVector& dexen) {
  if (dexen.size() == 1) {
    // only a dex left, nothing to do
    return 0;
  }

  // remove the canary class and this dex.
  size_t lowest_index = dexen.size();
  size_t num_removed = 0;
  for (size_t idx = 0; idx < dexen.size(); idx++) {
    if (dexen[idx].size() == 1) {
      auto it = dexen[idx].begin();
      if (is_canary(*it)) {
        dexen[idx].erase(it);
        num_removed++;
        if (idx < lowest_index) {
          lowest_index = idx;
        }
      }
    }
  }

  if (num_removed == 0) {
    // No empty dexes.
    return 0;
  }

  // Remove empty dex.
  dexen.erase(std::remove_if(dexen.begin(), dexen.end(),
                             [&](const std::vector<DexClass*>& dex) {
                               return dex.empty();
                             }),
              dexen.end());

  // The canary class name encodes the dex index. Since now some dexes are
  // removed, the dex index has been changed. Therefore, rewrite the canary
  // class if necessary.
  for (size_t i = lowest_index; i < dexen.size(); i++) {
    auto& dex = dexen.at(i);
    // Remove old canary class.
    dex.erase(std::remove_if(dex.begin(),
                             dex.end(),
                             [&](DexClass* c) { return is_canary(c); }),
              dex.end());

    // Insert a new canary class.
    DexClass* canary_cls = create_canary(i);
    for (auto* m : canary_cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      m->get_code()->build_cfg();
    }
    dex.insert(dex.begin(), canary_cls);
  }

  return num_removed;
}

void DexRemovalPass::sanity_check(Scope& original_scope,
                                  DexStoresVector& stores,
                                  size_t removed_num_dexes) {
  auto& root_store_new = stores.at(0);
  auto& root_dexen_new = root_store_new.get_dexen();

  std::unordered_set<DexClass*> original_scope_set(original_scope.begin(),
                                                   original_scope.end());
  auto new_scope = build_class_scope(stores);
  std::unordered_set<DexClass*> new_scope_set(new_scope.begin(),
                                              new_scope.end());
  always_assert(original_scope_set.size() ==
                new_scope_set.size() + removed_num_dexes);
  for (auto cls : new_scope_set) {
    always_assert(original_scope_set.count(cls));
  }

  // Check canaries being contiguous.
  for (size_t i = 0; i < root_dexen_new.size(); i++) {
    auto& dex = root_dexen_new.at(i);
    for (auto cls : dex) {
      if (is_canary(cls)) {
        std::string expected_name = get_canary_name(i);
        auto dtype = cls->get_type();
        auto oldname = dtype->get_name();
        always_assert(oldname->str() == expected_name);
        break;
      }
    }
  }
}

void DexRemovalPass::run_pass(DexStoresVector& stores,
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

  // First remove empty dexes if there is any.
  Scope oscope = build_class_scope(stores);
  auto& rstore = stores.at(0);
  auto& rdexen = rstore.get_dexen();
  size_t dex_removed = remove_empty_dexes(rdexen);
  if (dex_removed != 0) {
    sanity_check(oscope, stores, dex_removed);
  }

  while (true && m_class_reshuffle) {
    auto& root_store = stores.at(0);
    auto& root_dexen = root_store.get_dexen();
    if (root_dexen.size() == 1) {
      // only a dex left, nothing to do
      break;
    }

    Scope original_scope = build_class_scope(stores);
    TRACE(IDEXR, 1, "current number of dex is %zu", root_dexen.size());

    ReshuffleConfig config;
    const std::unordered_set<size_t> dynamically_dead_dexes;
    InterDexReshuffleImpl impl(conf, mgr, config, original_scope, root_dexen,
                               dynamically_dead_dexes);
    if (!impl.compute_dex_removal_plan()) {
      break;
    }
    impl.apply_plan();

    // Check root_store. There must be one dex should be removed.
    size_t num_removed = remove_empty_dexes(root_dexen);
    always_assert(num_removed == 1);

    sanity_check(original_scope, stores, num_removed);
  }

  auto& current_root_store = stores.at(0);
  auto& current_root_dexen = current_root_store.get_dexen();
  TRACE(IDEXR, 1,
        "The number of dexes after DexRemoval is %zu, and %zu dexes are "
        "removed.",
        current_root_dexen.size(), dex_removed);
  mgr.incr_metric("num_dexes", current_root_dexen.size());
  mgr.incr_metric("num_dexes_removed", dex_removed);
}

static DexRemovalPass s_pass;
