/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  std20::erase_if(stores, [&](auto& s) {
    if (s.is_generated()) {
      interdex->add_dexes_from_store(s);
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
  bind("order_interdex", true, m_order_interdex);
  bind("keep_primary_order", true, m_keep_primary_order);
  always_assert_log(m_keep_primary_order || m_normal_primary_dex,
                    "We always need to respect primary dex order if we treat "
                    "the primary dex as a special dex.");
  bind("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);

  bind("reserved_frefs", 0, m_reserve_refs.frefs,
       "A relief valve for field refs within each dex in case a legacy "
       "optimization introduces a new field reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_trefs", 0, m_reserve_refs.trefs,
       "A relief valve for type refs within each dex in case a legacy "
       "optimization introduces a new type reference without declaring it "
       "explicitly to the InterDex pass");
  bind("reserved_mrefs", 0, m_reserve_refs.mrefs,
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
  bind("minimize_cross_dex_refs_large_string_ref_weight",
       m_minimize_cross_dex_refs_config.large_string_ref_weight,
       m_minimize_cross_dex_refs_config.large_string_ref_weight);
  bind("minimize_cross_dex_refs_small_string_ref_weight",
       m_minimize_cross_dex_refs_config.small_string_ref_weight,
       m_minimize_cross_dex_refs_config.small_string_ref_weight);
  bind("minimize_cross_dex_refs_min_large_string_size",
       m_minimize_cross_dex_refs_config.min_large_string_size,
       m_minimize_cross_dex_refs_config.min_large_string_size);
  bind("minimize_cross_dex_refs_method_seed_weight",
       m_minimize_cross_dex_refs_config.method_seed_weight,
       m_minimize_cross_dex_refs_config.method_seed_weight);
  bind("minimize_cross_dex_refs_field_seed_weight",
       m_minimize_cross_dex_refs_config.field_seed_weight,
       m_minimize_cross_dex_refs_config.field_seed_weight);
  bind("minimize_cross_dex_refs_type_seed_weight",
       m_minimize_cross_dex_refs_config.type_seed_weight,
       m_minimize_cross_dex_refs_config.type_seed_weight);
  bind("minimize_cross_dex_refs_large_string_seed_weight",
       m_minimize_cross_dex_refs_config.large_string_seed_weight,
       m_minimize_cross_dex_refs_config.large_string_seed_weight);
  bind("minimize_cross_dex_refs_large_string_seed_weight",
       m_minimize_cross_dex_refs_config.small_string_seed_weight,
       m_minimize_cross_dex_refs_config.small_string_seed_weight);
  bind("minimize_cross_dex_refs_emit_json", false,
       m_minimize_cross_dex_refs_config.emit_json);
  bind("minimize_cross_dex_refs_explore_alternatives", 1,
       m_minimize_cross_dex_refs_explore_alternatives);

  bind("fill_last_coldstart_dex", m_fill_last_coldstart_dex,
       m_fill_last_coldstart_dex);

  bind("can_touch_coldstart_cls", false, m_can_touch_coldstart_cls);
  bind("can_touch_coldstart_extended_cls", false,
       m_can_touch_coldstart_extended_cls);
  bind("expect_order_list", false, m_expect_order_list);
  bind("methods_for_canary_clinit_reference", {},
       m_methods_for_canary_clinit_reference,
       "If set, canary classes will have a clinit generated which call the "
       "specified methods, if they exist");

  bind("transitively_close_interdex_order", m_transitively_close_interdex_order,
       m_transitively_close_interdex_order);

  trait(Traits::Pass::unique, true);
}

void InterDexPass::run_pass(
    const Scope& original_scope,
    const XStoreRefs& xstore_refs,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    DexStoresVector& stores,
    DexClassesVector& dexen,
    std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
    ConfigFiles& conf,
    PassManager& mgr,
    const ReserveRefsInfo& refs_info,
    ClassReferencesCache& cache) {
  mgr.set_metric(METRIC_LINEAR_ALLOC_LIMIT, m_linear_alloc_limit);
  mgr.set_metric(METRIC_RESERVED_FREFS, refs_info.frefs);
  mgr.set_metric(METRIC_RESERVED_TREFS, refs_info.trefs);
  mgr.set_metric(METRIC_RESERVED_MREFS, refs_info.mrefs);
  mgr.set_metric(METRIC_EMIT_CANARIES, m_emit_canaries);
  mgr.set_metric(METRIC_ORDER_INTERDEX, m_order_interdex);

  // Reflect configuration in stats.
  mgr.set_metric("config.normal_primary_dex", m_normal_primary_dex);
  mgr.set_metric("config.static_prune", m_static_prune);
  mgr.set_metric("config.keep_primary_order", m_keep_primary_order);
  mgr.set_metric("config.can_touch_coldstart_cls", m_can_touch_coldstart_cls);
  mgr.set_metric("config.can_touch_coldstart_extended_cls",
                 m_can_touch_coldstart_extended_cls);
  mgr.set_metric("config.minimize_cross_dex_refs", m_minimize_cross_dex_refs);
  mgr.set_metric("config.minimize_cross_dex_refs_explore_alternatives",
                 m_minimize_cross_dex_refs_explore_alternatives);
  mgr.set_metric("config.transitively_close_interdex_order",
                 m_transitively_close_interdex_order);

  bool force_single_dex = conf.get_json_config().get("force_single_dex", false);
  mgr.set_metric("config.force_single_dex", force_single_dex);

  InterDex interdex(
      original_scope, dexen, mgr.asset_manager(), conf, plugins,
      m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
      m_keep_primary_order, force_single_dex, m_order_interdex, m_emit_canaries,
      m_minimize_cross_dex_refs, m_fill_last_coldstart_dex,
      m_minimize_cross_dex_refs_config, refs_info, &xstore_refs,
      mgr.get_redex_options().min_sdk, m_methods_for_canary_clinit_reference,
      init_classes_with_side_effects, m_transitively_close_interdex_order,
      m_minimize_cross_dex_refs_explore_alternatives, cache);

  if (m_expect_order_list) {
    always_assert_log(
        !interdex.get_interdex_types().empty(),
        "Either no betamap was provided, or an empty list was passed in. FIX!");
  }

  interdex.run();
  treat_generated_stores(stores, &interdex);
  dexen = interdex.take_outdex();

  mgr.set_metric("root_store.dexes", dexen.size());
  redex_assert(dexen.size() == interdex.get_dex_info().size());
  for (size_t i = 0; i != dexen.size(); ++i) {
    std::string key_prefix = "root_store.dexes." + std::to_string(i) + ".";
    mgr.set_metric(key_prefix + "classes", dexen[i].size());
    auto& info = interdex.get_dex_info()[i];
    mgr.set_metric(key_prefix + "primary", info.primary);
    mgr.set_metric(key_prefix + "coldstart", info.coldstart);
    mgr.set_metric(key_prefix + "extended", info.extended);
    mgr.set_metric(key_prefix + "scroll", info.scroll);
    mgr.set_metric(key_prefix + "background", info.background);
    mgr.set_metric(key_prefix + "betamap_ordered", info.betamap_ordered);
  }

  auto final_scope = build_class_scope(stores);
  for (const auto& plugin : plugins) {
    plugin->cleanup(final_scope);
  }
  mgr.set_metric(METRIC_COLD_START_SET_DEX_COUNT,
                 interdex.get_num_cold_start_set_dexes());
  mgr.set_metric(METRIC_SCROLL_SET_DEX_COUNT, interdex.get_num_scroll_dexes());

  mgr.set_metric("transitive_added", interdex.get_transitive_closure_added());
  mgr.set_metric("transitive_moved", interdex.get_transitive_closure_moved());

  plugins.clear();

  const auto& cross_dex_ref_minimizer_stats =
      interdex.get_cross_dex_ref_minimizer_stats();
  mgr.set_metric(METRIC_REORDER_CLASSES, cross_dex_ref_minimizer_stats.classes);
  mgr.set_metric(METRIC_REORDER_RESETS, cross_dex_ref_minimizer_stats.resets);
  mgr.set_metric(METRIC_REORDER_REPRIORITIZATIONS,
                 cross_dex_ref_minimizer_stats.reprioritizations);
  const auto& seed_classes = cross_dex_ref_minimizer_stats.seed_classes;
  for (size_t i = 0; i < seed_classes.size(); ++i) {
    auto& p = seed_classes.at(i);
    std::string metric =
        METRIC_REORDER_CLASSES_SEEDS + std::to_string(i) + "_" + SHOW(p.first);
    mgr.set_metric(metric, p.second);
  }

  mgr.set_metric(METRIC_CURRENT_CLASSES_WHEN_EMITTING_REMAINING,
                 interdex.get_current_classes_when_emitting_remaining());

  auto& over = interdex.get_overflow_stats();
  mgr.set_metric("num_overflows.linear_alloc", over.linear_alloc_overflow);
  mgr.set_metric("num_overflows.method_refs", over.method_refs_overflow);
  mgr.set_metric("num_overflows.field_refs", over.field_refs_overflow);
  mgr.set_metric("num_overflows.type_refs", over.type_refs_overflow);
}

void InterDexPass::run_pass_on_nonroot_store(
    const Scope& original_scope,
    const XStoreRefs& xstore_refs,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    DexClassesVector& dexen,
    ConfigFiles& conf,
    PassManager& mgr,
    const ReserveRefsInfo& refs_info,
    ClassReferencesCache& cache) {
  // Setup default configs for non-root store
  // For now, no plugins configured for non-root stores to run.
  std::vector<std::unique_ptr<InterDexPassPlugin>> plugins;

  // Cross dex ref minimizers are disabled for non-root stores
  // TODO: Make this logic cleaner when these features get enabled for non-root
  //       stores. Would also need to clean up after it.
  cross_dex_ref_minimizer::CrossDexRefMinimizerConfig cross_dex_refs_config;

  // Initialize interdex and run for nonroot store
  InterDex interdex(
      original_scope, dexen, mgr.asset_manager(), conf, plugins,
      m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
      m_keep_primary_order, false /* force single dex */, m_order_interdex,
      false /* emit canaries */, false /* minimize_cross_dex_refs */,
      /* fill_last_coldstart_dex=*/false, cross_dex_refs_config, refs_info,
      &xstore_refs, mgr.get_redex_options().min_sdk,
      m_methods_for_canary_clinit_reference, init_classes_with_side_effects,
      m_transitively_close_interdex_order,
      m_minimize_cross_dex_refs_explore_alternatives, cache);

  interdex.run_on_nonroot_store();

  dexen = interdex.take_outdex();
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  Scope original_scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      original_scope, conf.create_init_class_insns());
  XStoreRefs xstore_refs(stores);

  // Setup all external plugins.
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));
  auto plugins = registry->create_plugins();

  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, conf);
  }

  ReserveRefsInfo refs_info = m_reserve_refs;
  refs_info += mgr.get_reserved_refs();

  ClassReferencesCache cache(original_scope);

  std::vector<DexStore*> parallel_stores;
  for (auto& store : stores) {
    if (store.is_root_store()) {
      run_pass(original_scope, xstore_refs, init_classes_with_side_effects,
               stores, store.get_dexen(), plugins, conf, mgr, refs_info, cache);
    } else if (!store.is_generated()) {
      parallel_stores.push_back(&store);
    }
  }

  workqueue_run<DexStore*>(
      [&](DexStore* store) {
        run_pass_on_nonroot_store(
            original_scope, xstore_refs, init_classes_with_side_effects,
            store->get_dexen(), conf, mgr, refs_info, cache);
        mgr.set_metric("nonroot_store." + store->get_name() + ".dexes",
                       store->get_dexen().size());
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
