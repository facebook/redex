/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

void load_roots_subtypes_as_merging_targets(const TypeSystem& type_system,
                                            ModelSpec* spec) {
  TypeSet merging_targets_set;
  std::unordered_set<DexType*> new_roots;
  for (auto root = spec->roots.begin(); root != spec->roots.end();) {
    if (is_interface(type_class(*root))) {
      const auto& implementors = type_system.get_implementors(*root);
      for (auto impl_type : implementors) {
        auto impl_cls = type_class(impl_type);
        // Note: Bellow is to simply make the logic unchange after the
        // refactoring.
        // Find the first internal class at the top of the type
        // hierarchy of the impl_cls. if it extends java.lang.Object and
        // implements only the *root interface, add the `impl_cls` to the
        // merging target.
        auto top_super_cls = impl_cls;
        while (top_super_cls->get_super_class() != type::java_lang_Object()) {
          auto super_cls = type_class(top_super_cls->get_super_class());
          if (!super_cls || super_cls->is_external()) {
            break;
          }
          top_super_cls = super_cls;
        }
        auto* intfs = top_super_cls->get_interfaces();
        if (top_super_cls->get_super_class() == type::java_lang_Object() &&
            (intfs->size() == 1 && *intfs->begin() == *root)) {
          new_roots.insert(impl_cls->get_super_class());
          spec->merging_targets.insert(impl_type);
        }
      }
      root = spec->roots.erase(root);
    } else {
      type_system.get_all_children(*root, merging_targets_set);
      root++;
    }
  }
  spec->roots.insert(new_roots.begin(), new_roots.end());
  spec->merging_targets.insert(merging_targets_set.begin(),
                               merging_targets_set.end());
}

} // namespace

namespace class_merging {

ModelStats merge_model(Scope& scope,
                       ConfigFiles& conf,
                       PassManager& mgr,
                       DexStoresVector& stores,
                       ModelSpec& spec) {
  always_assert(!spec.roots.empty());
  TypeSystem type_system(scope);
  if (spec.merging_targets.empty()) {
    load_roots_subtypes_as_merging_targets(type_system, &spec);
  }
  if (spec.merging_targets.empty()) {
    return ModelStats();
  }
  return merge_model(type_system, scope, conf, mgr, stores, spec);
}

ModelStats merge_model(const TypeSystem& type_system,
                       Scope& scope,
                       ConfigFiles& conf,
                       PassManager& mgr,
                       DexStoresVector& stores,
                       ModelSpec& spec) {
  set_up(conf);
  always_assert(s_is_initialized);
  TRACE(CLMG,
        2,
        "[ClassMerging] merging %s model merging targets %zu roots %zu",
        spec.name.c_str(),
        spec.merging_targets.size(),
        spec.roots.size());
  Timer t("erase_model");
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  XStoreRefs xstores(stores);
  auto refchecker =
      create_ref_checker(spec.per_dex_grouping, &xstores, conf, min_sdk);
  auto model =
      Model::build_model(scope, stores, conf, spec, type_system, *refchecker);
  ModelStats stats = model.get_model_stats();

  ModelMerger mm;
  auto merger_classes = mm.merge_model(scope, stores, conf, model);
  auto num_dedupped = method_dedup::dedup_constructors(merger_classes, scope);
  mm.increase_ctor_dedupped_stats(num_dedupped);
  stats += mm.get_model_stats();
  stats.update_redex_stats(spec.class_name_prefix, mgr);
  return stats;
}

} // namespace class_merging
