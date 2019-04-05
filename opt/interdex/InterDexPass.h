/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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

class InterDexPass : public Pass {
 public:
  InterDexPass() : Pass(INTERDEX_PASS_NAME) {
    std::unique_ptr<InterDexRegistry> plugin =
        std::make_unique<InterDexRegistry>();
    PluginRegistry::get().register_pass(INTERDEX_PASS_NAME, std::move(plugin));
  }

  void configure_pass(const JsonWrapper& jw) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
  bool m_normal_primary_dex;
  int64_t m_linear_alloc_limit;
  int64_t m_type_refs_limit;
  std::string m_mixed_mode_classes_file;
  bool m_can_touch_coldstart_cls;
  bool m_can_touch_coldstart_extended_cls;
  std::unordered_set<DexStatus, std::hash<int>> m_mixed_mode_dex_statuses;
  bool m_emit_scroll_set_marker;
  bool m_minimize_cross_dex_refs;
  CrossDexRefMinimizerConfig m_minimize_cross_dex_refs_config;
  CrossDexRelocatorConfig m_cross_dex_relocator_config;

  virtual void run_pass(DexStoresVector&,
                        DexClassesVector&,
                        ConfigFiles&,
                        PassManager&);
};

} // namespace interdex
