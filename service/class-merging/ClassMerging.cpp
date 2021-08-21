/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassMerging.h"

#include <iostream>

#include "ConfigFiles.h"
#include "ModelMerger.h"
#include "NormalizeConstructor.h"
#include "PassManager.h"
#include "RefChecker.h"
#include "Trace.h"

using namespace class_merging;

namespace {

bool s_is_initialized = false;

/*
 * Initialize a number of static states for the output mapping file and the
 * interdex group mapping.
 */
void set_up(ConfigFiles& conf) {
  if (s_is_initialized) {
    // Already initialized.
    return;
  }
  Model::build_interdex_groups(conf);
  s_is_initialized = true;
}

/**
 * Create a ref checker for checking cross-store references and Android SDK api
 * usages. When the per_dex_grouping is false, the create ref checker will check
 * cross-store references. When the per_dex_grouping is true, the checker
 * doesn't check cross-store reference and will let the class merging grouping
 * do the correct grouping for each dex.
 */
std::unique_ptr<RefChecker> create_ref_checker(const bool per_dex_grouping,
                                               XStoreRefs* xstores,
                                               ConfigFiles& conf,
                                               int min_sdk) {
  auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
  const api::AndroidSDK* min_sdk_api{nullptr};
  if (!min_sdk_api_file) {
    std::cerr
        << "[ClassMerging] Warning: needs Android SDK API list for android-"
        << min_sdk << std::endl;
  } else {
    min_sdk_api = &conf.get_android_sdk_api(min_sdk);
  }
  size_t store_id;
  if (per_dex_grouping) {
    xstores = nullptr;
    store_id = 0;
  } else {
    always_assert(xstores);
    // RefChecker store_idx is initialized with `largest_root_store_id()`, so
    // that it rejects all the references from stores with id larger than the
    // largest root_store id.
    store_id = xstores->largest_root_store_id();
  }
  return std::make_unique<RefChecker>(xstores, store_id, min_sdk_api);
}

} // namespace

namespace class_merging {

void merge_model(Scope& scope,
                 ConfigFiles& conf,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec) {
  TypeSystem type_system(scope);
  if (spec.merging_targets.empty()) {
    // TODO: change to unordered set.
    TypeSet merging_targets_set;
    for (const auto root : spec.roots) {
      type_system.get_all_children(root, merging_targets_set);
    }
    spec.merging_targets.insert(merging_targets_set.begin(),
                                merging_targets_set.end());
  }
  merge_model(type_system, scope, conf, mgr, stores, spec);
}

void merge_model(const TypeSystem& type_system,
                 Scope& scope,
                 ConfigFiles& conf,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec) {
  set_up(conf);
  always_assert(s_is_initialized);
  TRACE(CLMG, 2, "[ClassMerging] merging %s model", spec.name.c_str());
  Timer t("erase_model");
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  XStoreRefs xstores(stores);
  auto refchecker =
      create_ref_checker(spec.per_dex_grouping, &xstores, conf, min_sdk);
  auto model =
      Model::build_model(scope, stores, conf, spec, type_system, *refchecker);
  model.update_redex_stats(mgr);

  ModelMerger mm;
  auto merger_classes = mm.merge_model(scope, stores, conf, model);
  auto num_dedupped = method_dedup::dedup_constructors(merger_classes, scope);
  mm.increase_ctor_dedupped_stats(num_dedupped);
  mm.update_redex_stats(spec.class_name_prefix, mgr);
}

} // namespace class_merging
