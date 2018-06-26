/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "DexClass.h"
#include "Pass.h"
#include "PluginRegistry.h"
#include "Util.h"

#define METRIC_COLD_START_SET_DEX_COUNT "cold_start_set_dex_count"
#define METRIC_SCROLL_SET_DEX_COUNT "scroll_set_dex_count"

#define INTERDEX_PASS_NAME "InterDexPass"
#define INTERDEX_PLUGIN "InterDexPlugin"

enum DexStatus {
  FIRST_COLDSTART_DEX = 0,
  FIRST_EXTENDED_DEX = 1,
  SCROLL_DEX = 2,
};

class InterDexPassPlugin {
 public:
  // Run plugin initialization here. Pass should run this before running
  // its implementation
  virtual void configure(const Scope& original_scope, ConfigFiles& cfg) = 0;

  // Will prevent clazz from going into any output dex
  virtual bool should_skip_class(const DexClass* clazz) = 0;

  // Calculate the amount of refs that any classes from additional_classes
  // will add to the output dex (see below)
  virtual void gather_mrefs(const DexClass* cls,
                            std::vector<DexMethodRef*>& mrefs,
                            std::vector<DexFieldRef*>& frefs) = 0;

  // Return any new codegened classes should be added to the current dex
  virtual DexClasses additional_classes(const DexClassesVector& outdex,
                                        const DexClasses& classes) = 0;

  // Return classes that should be added at the end.
  virtual DexClasses leftover_classes() {
    DexClasses empty;
    return empty;
  }

  // Run plugin cleanup and finalization here. Pass should run this after
  // running its implementation
  virtual void cleanup(const std::vector<DexClass*>& scope) = 0;
  virtual ~InterDexPassPlugin(){};
};

typedef PluginEntry<InterDexPassPlugin> InterDexRegistry;

class InterDexPass : public Pass {
 public:
  InterDexPass() : Pass(INTERDEX_PASS_NAME) {
    std::unique_ptr<InterDexRegistry> plugin = std::make_unique<InterDexRegistry>();
    PluginRegistry::get().register_pass(INTERDEX_PASS_NAME, std::move(plugin));
  }

  virtual void configure_pass(const PassConfig& pc) override;

  virtual void run_pass(DexClassesVector&, Scope&, ConfigFiles&, PassManager&);
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  std::vector<std::unique_ptr<InterDexPassPlugin>> m_plugins;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
  bool m_normal_primary_dex;
  int64_t m_linear_alloc_limit;
  std::string m_mixed_mode_classes_file;
  bool m_can_touch_coldstart_cls;
  bool m_can_touch_coldstart_extended_cls;
  std::unordered_set<DexStatus, std::hash<int>> m_mixed_mode_dex_statuses;
  bool m_emit_scroll_set_marker;
};
