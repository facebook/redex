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
#include "InterDexPassPlugin.h"
#include "Pass.h"
#include "PluginRegistry.h"
#include "Util.h"

namespace interdex {

constexpr const char* INTERDEX_PASS_NAME = "InterDexPass";
constexpr const char* INTERDEX_PLUGIN = "InterDexPlugin";
constexpr const char* METRIC_COLD_START_SET_DEX_COUNT = "cold_start_set_dex_count";
constexpr const char* METRIC_SCROLL_SET_DEX_COUNT = "scroll_set_dex_count";

enum DexStatus {
  FIRST_COLDSTART_DEX = 0,
  FIRST_EXTENDED_DEX = 1,
  SCROLL_DEX = 2,
};

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
};

} // namespace interdex
