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
#include "StlUtil.h"
#include "WorkQueue.h"

namespace {

/**
 * Generated stores need to be added to the root store.
 * We achieve this, by adding all the dexes from those stores after the root
 * store.
 */
void treat_generated_stores(DexStoresVector& stores,
                            interdex::InterDex* interdex) {
  std20::erase_if(stores, [&](auto it) {
    if (it->is_generated()) {
      interdex->add_dexes_from_store(*it);
      return true;
    }
    return false;
  });
}

} // namespace

namespace interdex {

void InterDexPass::bind_config() {
  bind("static_prune", false, m_static_prune);
  bind("emit_canaries", true, m_emit_canaries);
  bind("normal_primary_dex", false, m_normal_primary_dex);
  bind("keep_primary_order", true, m_keep_primary_order);
  always_assert_log(m_keep_primary_order || m_normal_primary_dex,
                    "We always need to respect primary dex order if we treat "
                    "the primary dex as a special dex.");
  bind("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);

  bind("reserved_frefs", 0, m_reserved_frefs,
       "A relief valve for field refs within each dex in case a legacy "
       "optimization introduces a new field reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_trefs", 0, m_reserved_trefs,
       "A relief valve for type refs within each dex in case a legacy "
       "optimization introduces a new type reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_mrefs", 0, m_reserved_mrefs,
       "A relief valve for methods refs within each dex in case a legacy "
       "optimization introduces a new method reference without declaring it "
       "explicitly to the InterDex pass");

  bind("minimize_cross_dex_refs", false, m_minimize_cross_dex_refs);
  bind("minimize_cross_dex_refs_method_ref_weight",
       m_minimize_cross_dex_refs_config.method_ref_weight,
       m_minimize_cross_dex_refs_config.method_ref_weight);
  bind("minimize_cross_dex_refs_field_ref_weight",
       m_minimize_cross_dex_refs_config.field_ref_weight,
       m_minimize_cross_dex_refs_config.field_ref_weight);
  bind("minimize_cross_dex_refs_type_ref_weight",
       m_minimize_cross_dex_refs_config.type_ref_weight,
       m_minimize_cross_dex_refs_config.type_ref_weight);
  bind("minimize_cross_dex_refs_string_ref_weight",
       m_minimize_cross_dex_refs_config.string_ref_weight,
       m_minimize_cross_dex_refs_config.string_ref_weight);
  bind("minimize_cross_dex_refs_method_seed_weight",
       m_minimize_cross_dex_refs_config.method_seed_weight,
       m_minimize_cross_dex_refs_config.method_seed_weight);
  bind("minimize_cross_dex_refs_field_seed_weight",
       m_minimize_cross_dex_refs_config.field_seed_weight,
       m_minimize_cross_dex_refs_config.field_seed_weight);
  bind("minimize_cross_dex_refs_type_ref_weight",
       m_minimize_cross_dex_refs_config.type_seed_weight,
       m_minimize_cross_dex_refs_config.type_seed_weight);
  bind("minimize_cross_dex_refs_string_ref_weight",
       m_minimize_cross_dex_refs_config.string_seed_weight,
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
  bind("max_relocated_methods_per_class", 200,
       m_cross_dex_relocator_config.max_relocated_methods_per_class);

  bind("can_touch_coldstart_cls", false, m_can_touch_coldstart_cls);
  bind("can_touch_coldstart_extended_cls", false,
       m_can_touch_coldstart_extended_cls);
  bind("expect_order_list", false, m_expect_order_list);
  bind("sort_remaining_classes", false, m_sort_remaining_classes,
       "Whether to sort classes in non-primary, non-perf-sensitive dexes "
       "according to their inheritance hierarchies");

  trait(Traits::Pass::unique, true);
}

void InterDexPass::run_pass(
    const Scope& original_scope,
    const XStoreRefs& xstore_refs,
    DexStoresVector& stores,
    DexClassesVector& dexen,
    std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
    ConfigFiles& conf,
    PassManager& mgr,
    const ReserveRefsInfo& refs_info) {
  mgr.set_metric(METRIC_RESERVED_FREFS, refs_info.frefs);
  mgr.set_metric(METRIC_RESERVED_TREFS, refs_info.trefs);
  mgr.set_metric(METRIC_RESERVED_MREFS, refs_info.mrefs);

  bool force_single_dex = conf.get_json_config().get("force_single_dex", false);
  InterDex interdex(original_scope, dexen, mgr.asset_manager(), conf, plugins,
                    m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
                    m_keep_primary_order, force_single_dex, m_emit_canaries,
                    m_minimize_cross_dex_refs, m_minimize_cross_dex_refs_config,
                    m_cross_dex_relocator_config, refs_info.frefs,
                    refs_info.trefs, refs_info.mrefs, &xstore_refs,
                    mgr.get_redex_options().min_sdk, m_sort_remaining_classes);

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

  mgr.set_metric(METRIC_CURRENT_CLASSES_WHEN_EMITTING_REMAINING,
                 interdex.get_current_classes_when_emitting_remaining());
}

void InterDexPass::run_pass_on_nonroot_store(const Scope& original_scope,
                                             const XStoreRefs& xstore_refs,
                                             DexClassesVector& dexen,
                                             ConfigFiles& conf,
                                             PassManager& mgr,
                                             const ReserveRefsInfo& refs_info) {
  // Setup default configs for non-root store
  // For now, no plugins configured for non-root stores to run.
  std::vector<std::unique_ptr<InterDexPassPlugin>> plugins;

  // Cross dex ref minimizers are disabled for non-root stores
  // TODO: Make this logic cleaner when these features get enabled for non-root
  //       stores. Would also need to clean up after it.
  cross_dex_ref_minimizer::CrossDexRefMinimizerConfig cross_dex_refs_config;
  CrossDexRelocatorConfig cross_dex_relocator_config;

  // Initialize interdex and run for nonroot store
  InterDex interdex(original_scope, dexen, mgr.asset_manager(), conf, plugins,
                    m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
                    m_keep_primary_order, false /* force single dex */,
                    false /* emit canaries */,
                    false /* minimize_cross_dex_refs */, cross_dex_refs_config,
                    cross_dex_relocator_config, refs_info.frefs,
                    refs_info.trefs, refs_info.mrefs, &xstore_refs,
                    mgr.get_redex_options().min_sdk, m_sort_remaining_classes);

  interdex.run_on_nonroot_store();

  dexen = interdex.take_outdex();
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

  Scope original_scope = build_class_scope(stores);
  XStoreRefs xstore_refs(stores);

  // Setup all external plugins.
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));
  auto plugins = registry->create_plugins();

  ReserveRefsInfo refs_info(m_reserved_frefs, m_reserved_trefs,
                            m_reserved_mrefs);
  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, conf);
    refs_info.frefs += plugin->reserve_frefs();
    refs_info.trefs += plugin->reserve_trefs();
    refs_info.mrefs += plugin->reserve_mrefs();
  }

  std::vector<DexStore*> parallel_stores;
  for (auto& store : stores) {
    if (store.is_root_store()) {
      run_pass(original_scope, xstore_refs, stores, store.get_dexen(), plugins,
               conf, mgr, refs_info);
    } else if (!store.is_generated()) {
      parallel_stores.push_back(&store);
    }
  }

  workqueue_run<DexStore*>(
      [&](DexStore* store) {
        run_pass_on_nonroot_store(original_scope, xstore_refs,
                                  store->get_dexen(), conf, mgr, refs_info);
      },
      parallel_stores);

  ++m_run;
  // For the last invocation, record that final interdex has been done.
  if (m_eval == m_run) {
    mgr.record_running_interdex();
  }
}

static InterDexPass s_pass;

} // namespace interdex
