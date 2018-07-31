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

constexpr const char* ROOT_STORE_NAME = "classes";

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

  jw.get("emit_scroll_set_marker", false, m_emit_scroll_set_marker);

  always_assert_log(
      !m_can_touch_coldstart_cls || m_can_touch_coldstart_extended_cls,
      "can_touch_coldstart_extended_cls needs to be true, when we can touch "
      "coldstart classes. Please set can_touch_coldstart_extended_cls "
      "to true\n");

  std::vector<std::string> mixed_mode_dexes;
  jw.get("mixed_mode_dexes", {}, mixed_mode_dexes);
  m_mixed_mode_dex_statuses = get_mixed_mode_dex_statuses(mixed_mode_dexes);
}

void InterDexPass::run_pass(DexClassesVector& dexen,
                            Scope& original_scope,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));

  auto plugins = registry->create_plugins();
  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, cfg);
  }

  InterDex interdex(dexen, m_mixed_mode_classes_file, m_mixed_mode_dex_statuses,
                    mgr.apk_manager(), cfg, plugins, m_linear_alloc_limit,
                    m_static_prune, m_normal_primary_dex,
                    m_can_touch_coldstart_cls,
                    m_can_touch_coldstart_extended_cls,
                    m_emit_scroll_set_marker, m_emit_canaries);
  dexen = interdex.run();

  for (const auto& plugin : plugins) {
    plugin->cleanup(original_scope);
  }
  mgr.incr_metric(METRIC_COLD_START_SET_DEX_COUNT,
                  interdex.get_num_cold_start_set_dexes());

  plugins.clear();
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

  auto original_scope = build_class_scope(stores);
  for (auto& store : stores) {
    if (store.get_name() == ROOT_STORE_NAME) {
      run_pass(store.get_dexen(), original_scope, cfg, mgr);
    }
  }
}

static InterDexPass s_pass;

} // namespace interdex
