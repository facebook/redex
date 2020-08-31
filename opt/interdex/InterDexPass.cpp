/*
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
#include "Show.h"
#include "WorkQueue.h"

namespace {

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

void InterDexPass::bind_config() {
  bind("static_prune", false, m_static_prune);
  bind("emit_canaries", true, m_emit_canaries);
  bind("normal_primary_dex", false, m_normal_primary_dex);
  bind("linear_alloc_limit", {11600 * 1024}, m_linear_alloc_limit);

  bind("reserved_frefs", {0}, m_reserved_frefs,
       "A relief valve for field refs within each dex in case a legacy "
       "optimization introduces a new field reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_trefs", {0}, m_reserved_trefs,
       "A relief valve for type refs within each dex in case a legacy "
       "optimization introduces a new type reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_mrefs", {0}, m_reserved_mrefs,
       "A relief valve for methods refs within each dex in case a legacy "
       "optimization introduces a new method reference without declaring it "
       "explicitly to the InterDex pass");

  bind("minimize_cross_dex_refs", false, m_minimize_cross_dex_refs);
  bind("minimize_cross_dex_refs_method_ref_weight", {100},
       m_minimize_cross_dex_refs_config.method_ref_weight);
  bind("minimize_cross_dex_refs_field_ref_weight", {90},
       m_minimize_cross_dex_refs_config.field_ref_weight);
  bind("minimize_cross_dex_refs_type_ref_weight", {100},
       m_minimize_cross_dex_refs_config.type_ref_weight);
  bind("minimize_cross_dex_refs_string_ref_weight", {90},
       m_minimize_cross_dex_refs_config.string_ref_weight);
  bind("minimize_cross_dex_refs_method_seed_weight", {100},
       m_minimize_cross_dex_refs_config.method_seed_weight);
  bind("minimize_cross_dex_refs_field_seed_weight", {20},
       m_minimize_cross_dex_refs_config.field_seed_weight);
  bind("minimize_cross_dex_refs_type_ref_weight", {30},
       m_minimize_cross_dex_refs_config.type_seed_weight);
  bind("minimize_cross_dex_refs_string_ref_weight", {20},
       m_minimize_cross_dex_refs_config.string_seed_weight);
  bind("minimize_cross_dex_refs_relocate_static_methods", false,
       m_cross_dex_relocator_config.relocate_static_methods);
  bind("minimize_cross_dex_refs_relocate_non_static_direct_methods", false,
       m_cross_dex_relocator_config.relocate_non_static_direct_methods);
  bind("minimize_cross_dex_refs_relocate_virtual_methods", false,
       m_cross_dex_relocator_config.relocate_virtual_methods);

  // The actual number of relocated methods per class tends to be just a
  // fraction of this number, as relocated methods get re-relocated back into
  // their original class when they end up in the same dex.
  bind("max_relocated_methods_per_class", {200},
       m_cross_dex_relocator_config.max_relocated_methods_per_class);

  bind("can_touch_coldstart_cls", false, m_can_touch_coldstart_cls);
  bind("can_touch_coldstart_extended_cls", false,
       m_can_touch_coldstart_extended_cls);
  bind("expect_order_list", false, m_expect_order_list);

  trait(Traits::Pass::unique, true);
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            DexClassesVector& dexen,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  // Setup all external plugins.
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));

  auto plugins = registry->create_plugins();
  size_t reserve_frefs = m_reserved_frefs;
  size_t reserve_trefs = m_reserved_trefs;
  size_t reserve_mrefs = m_reserved_mrefs;
  auto original_scope = build_class_scope(stores);
  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, conf);
    reserve_frefs += plugin->reserve_frefs();
    reserve_trefs += plugin->reserve_trefs();
    reserve_mrefs += plugin->reserve_mrefs();
  }
  mgr.set_metric(METRIC_RESERVED_FREFS, reserve_frefs);
  mgr.set_metric(METRIC_RESERVED_TREFS, reserve_trefs);
  mgr.set_metric(METRIC_RESERVED_MREFS, reserve_mrefs);

  bool force_single_dex = conf.get_json_config().get("force_single_dex", false);
  XStoreRefs xstore_refs(stores);
  InterDex interdex(original_scope, dexen, mgr.apk_manager(), conf, plugins,
                    m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
                    force_single_dex, m_emit_canaries,
                    m_minimize_cross_dex_refs, m_minimize_cross_dex_refs_config,
                    m_cross_dex_relocator_config, reserve_frefs, reserve_trefs,
                    reserve_mrefs, &xstore_refs,
                    mgr.get_redex_options().min_sdk);

  if (m_expect_order_list) {
    always_assert_log(
        !interdex.get_interdex_types().empty(),
        "Either no betamap was provided, or an empty list was passed in. FIX!");
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

void InterDexPass::run_pass_on_nonroot_store(DexStoresVector& stores,
                                             DexClassesVector& dexen,
                                             ConfigFiles& conf,
                                             PassManager& mgr) {
  auto original_scope = build_class_scope(stores);

  // Setup default configs for non-root store
  // For now, no plugins configured for non-root stores
  std::vector<std::unique_ptr<InterDexPassPlugin>> plugins;
  size_t reserve_frefs = m_reserved_frefs;
  size_t reserve_trefs = m_reserved_trefs;
  size_t reserve_mrefs = m_reserved_mrefs;

  // Cross dex ref minimizers are disabled for non-root stores
  // TODO: Make this logic cleaner when these features get enabled for non-root
  // stores
  CrossDexRefMinimizerConfig cross_dex_refs_config;
  CrossDexRelocatorConfig cross_dex_relocator_config;

  // Initialize interdex and run for nonroot store
  XStoreRefs xstore_refs(stores);
  InterDex interdex(original_scope, dexen, mgr.apk_manager(), conf, plugins,
                    m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
                    false /* force single dex */, false /* emit canaries */,
                    false /* minimize_cross_dex_refs */, cross_dex_refs_config,
                    cross_dex_relocator_config, reserve_frefs, reserve_trefs,
                    reserve_mrefs, &xstore_refs,
                    mgr.get_redex_options().min_sdk);

  interdex.run_on_nonroot_store();

  auto final_scope = build_class_scope(stores);
  interdex.cleanup(final_scope);
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(
        IDEX, 1,
        "InterDexPass not run because no ProGuard configuration was provided.");
    return;
  }

  auto wq = workqueue_foreach<DexStore*>([&](DexStore* store) {
    run_pass_on_nonroot_store(stores, store->get_dexen(), conf, mgr);
  });
  for (auto& store : stores) {
    if (store.is_root_store()) {
      run_pass(stores, store.get_dexen(), conf, mgr);
    } else if (!store.is_generated()) {
      wq.add_item(&store);
    }
  }
  wq.run_all();
}

static InterDexPass s_pass;

} // namespace interdex
