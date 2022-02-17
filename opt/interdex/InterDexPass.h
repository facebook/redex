/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "InterDex.h"
#include "InterDexPassPlugin.h"
#include "Pass.h"
#include "PluginRegistry.h"

namespace interdex {

constexpr const char* INTERDEX_PASS_NAME = "InterDexPass";
constexpr const char* INTERDEX_PLUGIN = "InterDexPlugin";
constexpr const char* METRIC_COLD_START_SET_DEX_COUNT =
    "cold_start_set_dex_count";
constexpr const char* METRIC_SCROLL_SET_DEX_COUNT = "scroll_set_dex_count";

constexpr const char* METRIC_REORDER_CLASSES = "num_reorder_classes";
constexpr const char* METRIC_REORDER_RESETS = "num_reorder_resets";
constexpr const char* METRIC_REORDER_REPRIORITIZATIONS =
    "num_reorder_reprioritization";
constexpr const char* METRIC_REORDER_CLASSES_WORST = "reorder_classes_worst";

constexpr const char* METRIC_CLASSES_ADDED_FOR_RELOCATED_METHODS =
    "num_classes_added_for_relocated_methods";
constexpr const char* METRIC_RELOCATABLE_STATIC_METHODS =
    "num_relocatable_static_methods";
constexpr const char* METRIC_RELOCATABLE_NON_STATIC_DIRECT_METHODS =
    "num_relocatable_non_static_direct_methods";
constexpr const char* METRIC_RELOCATABLE_VIRTUAL_METHODS =
    "num_relocatable_virtual_methods";
constexpr const char* METRIC_RELOCATED_STATIC_METHODS =
    "num_relocated_static_methods";
constexpr const char* METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS =
    "num_relocated_non_static_direct_methods";
constexpr const char* METRIC_RELOCATED_VIRTUAL_METHODS =
    "num_relocated_virtual_methods";
constexpr const char* METRIC_CURRENT_CLASSES_WHEN_EMITTING_REMAINING =
    "num_current_classes_when_emitting_remaining";

constexpr const char* METRIC_LINEAR_ALLOC_LIMIT = "linear_alloc_limit";
constexpr const char* METRIC_RESERVED_FREFS = "reserved_frefs";
constexpr const char* METRIC_RESERVED_TREFS = "reserved_trefs";
constexpr const char* METRIC_RESERVED_MREFS = "reserved_mrefs";
constexpr const char* METRIC_EMIT_CANARIES = "emit_canaries";

struct ReserveRefsInfo {
  int64_t frefs;
  int64_t trefs;
  int64_t mrefs;

  ReserveRefsInfo(int64_t _frefs, int64_t _trefs, int64_t _mrefs)
      : frefs(_frefs), trefs(_trefs), mrefs(_mrefs) {}
};

class InterDexPass : public Pass {
 public:
  explicit InterDexPass(bool register_plugins = true)
      : Pass(INTERDEX_PASS_NAME) {
    if (register_plugins) {
      std::unique_ptr<InterDexRegistry> plugin =
          std::make_unique<InterDexRegistry>();
      PluginRegistry::get().register_pass(INTERDEX_PASS_NAME,
                                          std::move(plugin));
    }
  }

  void bind_config() override;

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override {
    ++m_eval;
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
  bool m_normal_primary_dex;
  bool m_keep_primary_order;
  int64_t m_linear_alloc_limit;
  int64_t m_reserved_frefs;
  int64_t m_reserved_trefs;
  int64_t m_reserved_mrefs;
  bool m_can_touch_coldstart_cls;
  bool m_can_touch_coldstart_extended_cls;
  bool m_minimize_cross_dex_refs;
  bool m_fill_last_coldstart_dex{false};
  cross_dex_ref_minimizer::CrossDexRefMinimizerConfig
      m_minimize_cross_dex_refs_config;
  CrossDexRelocatorConfig m_cross_dex_relocator_config;
  bool m_expect_order_list;
  bool m_sort_remaining_classes;
  std::vector<std::string> m_methods_for_canary_clinit_reference;
  bool m_transitively_close_interdex_order{true};

  size_t m_run{0}; // Which iteration of `run_pass`.
  size_t m_eval{0}; // How many `eval_pass` iterations.

  virtual void run_pass(const Scope&,
                        const XStoreRefs&,
                        const init_classes::InitClassesWithSideEffects&
                            init_classes_with_side_effects,
                        DexStoresVector&,
                        DexClassesVector&,
                        std::vector<std::unique_ptr<InterDexPassPlugin>>&,
                        ConfigFiles&,
                        PassManager&,
                        const ReserveRefsInfo&);

  void run_pass_on_nonroot_store(const Scope&,
                                 const XStoreRefs&,
                                 const init_classes::InitClassesWithSideEffects&
                                     init_classes_with_side_effects,
                                 DexClassesVector&,
                                 ConfigFiles&,
                                 PassManager&,
                                 const ReserveRefsInfo&);
};

} // namespace interdex
