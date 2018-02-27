/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <vector>

#include "DexClass.h"
#include "Pass.h"
#include "PluginRegistry.h"
#include "Util.h"

#define METRIC_COLD_START_SET_DEX_COUNT "cold_start_set_dex_count"

#define INTERDEX_PASS_NAME "InterDexPass"
#define INTERDEX_PLUGIN "InterDexPlugin"

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

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("static_prune", false, m_static_prune);
    pc.get("emit_canaries", true, m_emit_canaries);
    pc.get("normal_primary_dex", false, m_normal_primary_dex);
    pc.get("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);
    pc.get("scroll_classes_file", "", m_scroll_classes_file);
  }

  virtual void run_pass(DexClassesVector&, Scope&, ConfigFiles&, PassManager&);
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void get_scroll_classes();

  std::vector<std::unique_ptr<InterDexPassPlugin>> m_plugins;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
  bool m_normal_primary_dex;
  int64_t m_linear_alloc_limit;
  std::string m_scroll_classes_file;
};
