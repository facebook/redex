/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassMerging.h"

#include <iostream>

#include "ClassAssemblingUtils.h"
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

std::unique_ptr<RefChecker> ref_checker_for_root_store(XStoreRefs* xstores,
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
  // RefChecker store_idx is initialized with `largest_root_store_id()`, so that
  // it rejects all the references from stores with id larger than the largest
  // root_store id.
  return std::make_unique<RefChecker>(
      xstores, xstores->largest_root_store_id(), min_sdk_api);
}

} // namespace

namespace class_merging {

void merge_model(Scope& scope,
                 ConfigFiles& conf,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec) {
  set_up(conf);
  always_assert(s_is_initialized);
  handle_interface_as_root(spec, scope, stores);
  TRACE(CLMG, 2, "[ClassMerging] merging %s model", spec.name.c_str());
  Timer t("erase_model");
  for (const auto root : spec.roots) {
    always_assert(!is_interface(type_class(root)));
  }
  TypeSystem type_system(scope);
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  XStoreRefs xstores(stores);
  auto refchecker = ref_checker_for_root_store(&xstores, conf, min_sdk);
  auto model =
      Model::build_model(scope, conf, stores, spec, type_system, *refchecker);
  model.update_redex_stats(mgr);

  ModelMerger mm;
  auto merger_classes = mm.merge_model(scope, stores, conf, model);
  auto num_dedupped = method_dedup::dedup_constructors(merger_classes, scope);
  mm.increase_ctor_dedupped_stats(num_dedupped);
  mm.update_redex_stats(spec.class_name_prefix, mgr);
}

} // namespace class_merging
