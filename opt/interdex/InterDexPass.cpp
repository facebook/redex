/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexPass.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"

namespace {

std::unordered_set<interdex::DexStatus, std::hash<int>>
get_mixed_mode_dex_statuses(
    const std::vector<std::string>& mixed_mode_dex_statuses) {
  std::unordered_set<interdex::DexStatus, std::hash<int>> res;

  static std::unordered_map<std::string, interdex::DexStatus> string_to_status =
      {{"first_coldstart_dex", interdex::FIRST_COLDSTART_DEX},
       {"first_extended_dex", interdex::FIRST_EXTENDED_DEX},
       {"scroll_dex", interdex::SCROLL_DEX}};

  for (const std::string& mixed_mode_dex : mixed_mode_dex_statuses) {
    always_assert_log(string_to_status.count(mixed_mode_dex),
                      "Dex Status %s not found. Please check the list "
                      "of accepted statuses.\n",
                      mixed_mode_dex.c_str());
    res.emplace(string_to_status.at(mixed_mode_dex));
  }

  return res;
}

std::unordered_set<DexClass*> get_mixed_mode_classes(
    const std::string& mixed_mode_classes_file) {
  std::ifstream input(mixed_mode_classes_file.c_str(), std::ifstream::in);
  std::unordered_set<DexClass*> mixed_mode_classes;

  if (!input) {
    TRACE(IDEX, 2, "Mixed mode class file: %s : not found\n",
          mixed_mode_classes_file.c_str());
    return mixed_mode_classes;
  }

  std::string class_name;
  while (input >> class_name) {
    auto type = DexType::get_type(class_name.c_str());
    if (!type) {
      TRACE(IDEX, 4, "Couldn't find DexType for mixed mode class: %s\n",
            class_name.c_str());
      continue;
    }
    auto cls = type_class(type);
    if (!cls) {
      TRACE(IDEX, 4, "Couldn't find DexClass for mixed mode class: %s\n",
            class_name.c_str());
      continue;
    }
    if (mixed_mode_classes.count(cls)) {
      TRACE(IDEX, 2, "Duplicate classes found in mixed mode list\n");
      exit(1);
    }
    TRACE(IDEX, 4, "Adding %s in mixed mode list\n", SHOW(cls));
    mixed_mode_classes.emplace(cls);
  }
  input.close();

  return mixed_mode_classes;
}

std::unordered_set<DexClass*> get_mixed_mode_classes(
    const DexClassesVector& dexen, const std::string& mixed_mode_classes_file) {
  // If we have the list of the classes defined, use it.
  if (!mixed_mode_classes_file.empty()) {
    return get_mixed_mode_classes(mixed_mode_classes_file);
  }

  // Otherwise, check for classes that have the mix mode flag set.
  std::unordered_set<DexClass*> mixed_mode_classes;
  for (const auto& dex : dexen) {
    for (const auto& cls : dex) {
      if (cls->rstate.has_mix_mode()) {
        TRACE(IDEX, 4, "Adding class %s to the scroll list\n", SHOW(cls));
        mixed_mode_classes.emplace(cls);
      }
    }
  }
  return mixed_mode_classes;
}

/**
 * Generated stores need to be added to the root store.
 * We achieve this, by adding all the dexes from those stores after the root
 * store.
 */
void treat_generated_stores(DexStoresVector& stores,
                            interdex::InterDex* interdex) {
  auto store_it = stores.begin();
  while (store_it != stores.end()) {
    if (store_it->is_generated()) {
      interdex->add_dexes_from_store(*store_it);
      store_it = stores.erase(store_it);
    } else {
      store_it++;
    }
  }
}

} // namespace

namespace interdex {

void InterDexPass::configure_pass(const JsonWrapper& jw) {
  jw.get("static_prune", false, m_static_prune);
  jw.get("emit_canaries", true, m_emit_canaries);
  jw.get("normal_primary_dex", false, m_normal_primary_dex);
  jw.get("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);
  jw.get("scroll_classes_file", "", m_mixed_mode_classes_file);

  jw.get("can_touch_coldstart_cls", false, m_can_touch_coldstart_cls);
  jw.get("can_touch_coldstart_extended_cls", false,
         m_can_touch_coldstart_extended_cls);
  always_assert_log(
      !m_can_touch_coldstart_cls || m_can_touch_coldstart_extended_cls,
      "can_touch_coldstart_extended_cls needs to be true, when we can touch "
      "coldstart classes. Please set can_touch_coldstart_extended_cls "
      "to true\n");

  std::vector<std::string> mixed_mode_dexes;
  jw.get("mixed_mode_dexes", {}, mixed_mode_dexes);
  m_mixed_mode_dex_statuses = get_mixed_mode_dex_statuses(mixed_mode_dexes);

  // Default to maximum number of type refs per dex, as allowed by Android.
  // Notes: This flag was added to work around a bug in AOSP described in
  //        https://phabricator.internmc.facebook.com/P60294798 and for this
  //        it should be set to 1 << 15.
  jw.get("type_refs_limit", 1 << 16, m_type_refs_limit);

  jw.get("minimize_cross_dex_refs", false, m_minimize_cross_dex_refs);
  jw.get("minimize_cross_dex_refs_method_ref_weight", 100,
         m_minimize_cross_dex_refs_config.method_ref_weight);
  jw.get("minimize_cross_dex_refs_field_ref_weight", 90,
         m_minimize_cross_dex_refs_config.field_ref_weight);
  jw.get("minimize_cross_dex_refs_type_ref_weight", 100,
         m_minimize_cross_dex_refs_config.type_ref_weight);
  jw.get("minimize_cross_dex_refs_string_ref_weight", 90,
         m_minimize_cross_dex_refs_config.string_ref_weight);
  jw.get("minimize_cross_dex_refs_method_seed_weight", 100,
         m_minimize_cross_dex_refs_config.method_seed_weight);
  jw.get("minimize_cross_dex_refs_field_seed_weight", 20,
         m_minimize_cross_dex_refs_config.field_seed_weight);
  jw.get("minimize_cross_dex_refs_type_ref_weight", 30,
         m_minimize_cross_dex_refs_config.type_seed_weight);
  jw.get("minimize_cross_dex_refs_string_ref_weight", 20,
         m_minimize_cross_dex_refs_config.string_seed_weight);
  jw.get("minimize_cross_dex_refs_relocate_static_methods", false,
         m_cross_dex_relocator_config.relocate_static_methods);
  jw.get("minimize_cross_dex_refs_relocate_non_static_direct_methods", false,
         m_cross_dex_relocator_config.relocate_non_static_direct_methods);
  jw.get("minimize_cross_dex_refs_relocate_virtual_methods", false,
         m_cross_dex_relocator_config.relocate_virtual_methods);

  // The actual number of relocated methods per class tends to be just a
  // fraction of this number, as relocated methods get re-relocated back into
  // their original class when they end up in the same dex.
  jw.get("max_relocated_methods_per_class", 200,
         m_cross_dex_relocator_config.max_relocated_methods_per_class);
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            DexClassesVector& dexen,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  // Setup all external plugins.
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));

  auto plugins = registry->create_plugins();
  size_t reserve_mrefs = 0;
  auto original_scope = build_class_scope(stores);
  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, cfg);
    reserve_mrefs += plugin->reserve_mrefs();
  }

  InterDex interdex(
      original_scope, dexen, mgr.apk_manager(), cfg, plugins,
      m_linear_alloc_limit, m_type_refs_limit, m_static_prune,
      m_normal_primary_dex, m_emit_scroll_set_marker, m_emit_canaries,
      m_minimize_cross_dex_refs, m_minimize_cross_dex_refs_config,
      m_cross_dex_relocator_config, reserve_mrefs);

  // If we have a list of pre-defined dexes for mixed mode, that has priority.
  // Otherwise, we check if we have a list of pre-defined classes.
  if (m_mixed_mode_dex_statuses.size()) {
    TRACE(IDEX, 3, "Will compile pre-defined dex(es)\n");
    interdex.set_mixed_mode_dex_statuses(std::move(m_mixed_mode_dex_statuses));
  } else {
    auto mixed_mode_classes =
        get_mixed_mode_classes(dexen, m_mixed_mode_classes_file);
    if (mixed_mode_classes.size() > 0) {
      TRACE(IDEX, 3, "[mixed mode]: %d pre-computed mixed mode classes\n",
            mixed_mode_classes.size());
      interdex.set_mixed_mode_classes(std::move(mixed_mode_classes),
                                      m_can_touch_coldstart_cls,
                                      m_can_touch_coldstart_extended_cls);
    }
  }

  interdex.run();
  treat_generated_stores(stores, &interdex);
  dexen = interdex.take_outdex();

  auto final_scope = build_class_scope(stores);
  interdex.cleanup(final_scope);
  for (const auto& plugin : plugins) {
    plugin->cleanup(final_scope);
  }
  mgr.set_metric(METRIC_COLD_START_SET_DEX_COUNT,
                 interdex.get_num_cold_start_set_dexes());
  mgr.set_metric(METRIC_SCROLL_SET_DEX_COUNT, interdex.get_num_scroll_dexes());

  plugins.clear();

  const auto& cross_dex_ref_minimizer_stats =
      interdex.get_cross_dex_ref_minimizer_stats();
  mgr.set_metric(METRIC_REORDER_CLASSES, cross_dex_ref_minimizer_stats.classes);
  mgr.set_metric(METRIC_REORDER_RESETS, cross_dex_ref_minimizer_stats.resets);
  mgr.set_metric(METRIC_REORDER_REPRIORITIZATIONS,
                 cross_dex_ref_minimizer_stats.reprioritizations);
  const auto& worst_classes = cross_dex_ref_minimizer_stats.worst_classes;
  for (size_t i = 0; i < worst_classes.size(); ++i) {
    auto& p = worst_classes.at(i);
    std::string metric =
        METRIC_REORDER_CLASSES_WORST + std::to_string(i) + "_" + SHOW(p.first);
    mgr.set_metric(metric, p.second);
  }

  const auto cross_dex_relocator_stats =
      interdex.get_cross_dex_relocator_stats();
  mgr.set_metric(METRIC_CLASSES_ADDED_FOR_RELOCATED_METHODS,
                 cross_dex_relocator_stats.classes_added_for_relocated_methods);
  mgr.set_metric(METRIC_RELOCATABLE_STATIC_METHODS,
                 cross_dex_relocator_stats.relocatable_static_methods);
  mgr.set_metric(
      METRIC_RELOCATABLE_NON_STATIC_DIRECT_METHODS,
      cross_dex_relocator_stats.relocatable_non_static_direct_methods);
  mgr.set_metric(METRIC_RELOCATABLE_VIRTUAL_METHODS,
                 cross_dex_relocator_stats.relocatable_virtual_methods);
  mgr.set_metric(METRIC_RELOCATED_STATIC_METHODS,
                 cross_dex_relocator_stats.relocated_static_methods);
  mgr.set_metric(METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS,
                 cross_dex_relocator_stats.relocated_non_static_direct_methods);
  mgr.set_metric(METRIC_RELOCATED_VIRTUAL_METHODS,
                 cross_dex_relocator_stats.relocated_virtual_methods);
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(
        IDEX, 1,
        "InterDexPass not run because no ProGuard configuration was provided.");
    return;
  }

  for (auto& store : stores) {
    if (store.is_root_store()) {
      run_pass(stores, store.get_dexen(), cfg, mgr);
    }
  }
}

static InterDexPass s_pass;

} // namespace interdex
