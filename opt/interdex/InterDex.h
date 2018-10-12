/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ApkManager.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "InterDexPassPlugin.h"
#include "MixedModeInfo.h"

namespace interdex {

class InterDex {
 public:
  InterDex(const DexClassesVector& dexen,
           ApkManager& apk_manager,
           ConfigFiles& cfg,
           std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
           int64_t linear_alloc_limit,
           bool static_prune_classes,
           bool normal_primary_dex,
           bool emit_scroll_set_marker,
           bool emit_canaries)
      : m_dexen(dexen),
        m_apk_manager(apk_manager),
        m_cfg(cfg),
        m_plugins(plugins),
        m_static_prune_classes(static_prune_classes),
        m_normal_primary_dex(normal_primary_dex),
        m_emit_canaries(emit_canaries) {
    m_dexes_structure.set_linear_alloc_limit(linear_alloc_limit);
  }

  void set_mixed_mode_dex_statuses(
      std::unordered_set<DexStatus, std::hash<int>>&& mixed_mode_dex_statuses) {
    m_mixed_mode_info.set_mixed_mode_dex_statuses(
        std::move(mixed_mode_dex_statuses));
  }

  void set_mixed_mode_classes(
      std::unordered_set<DexClass*>&& mixed_mode_classes,
      bool can_touch_coldstart_set,
      bool can_touch_coldstart_extended_set) {
    m_mixed_mode_info.set_mixed_mode_classes(std::move(mixed_mode_classes),
                                             can_touch_coldstart_set,
                                             can_touch_coldstart_extended_set);
  }

  size_t get_num_cold_start_set_dexes() const {
    return m_dexes_structure.get_num_coldstart_dexes();
  }

  size_t get_num_scroll_dexes() const {
    return m_dexes_structure.get_num_scroll_dexes();
  }

  /**
   * Only call this if you know what you are doing.
   * This will leave the current instance is in an unusable state.
   */
  DexClassesVector take_outdex() { return std::move(m_outdex); }

  void run();
  void add_dexes_from_store(const DexStore& store);

 private:
  void emit_class(const DexInfo& dex_info, DexClass* clazz, bool check_if_skip);
  void emit_primary_dex(
      const DexClasses& primary_dex,
      const std::vector<DexType*>& interdex_order,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void emit_interdex_classes(
      const std::vector<DexType*>& interdex_types,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void emit_mixed_mode_classes(const std::vector<DexType*>& interdexorder,
                               bool can_touch_interdex_order);
  void flush_out_dex(DexInfo dex_info);
  bool is_mixed_mode_dex(const DexInfo& dex_info);

  /**
   * Returns a list of coldstart types. It will only contain:
   * * classes that still exist in the current scope
   * * + a "fake" type for each of the class markers (ex: DexEndMarker etc)
   */
  std::vector<DexType*> get_interdex_types(const Scope& scope);

  /**
   * Makes sure that classes in the dex end up in the interdex list.
   * For the classes that aren't already in the list, it adds them at
   * the beginning.
   */
  void update_interdexorder(const DexClasses& dex,
                            std::vector<DexType*>* interdex_types);

  const DexClassesVector& m_dexen;
  DexClassesVector m_outdex;
  ApkManager& m_apk_manager;
  ConfigFiles& m_cfg;
  std::vector<std::unique_ptr<InterDexPassPlugin>>& m_plugins;
  bool m_static_prune_classes;
  bool m_normal_primary_dex;
  bool m_emit_canaries;

  MixedModeInfo m_mixed_mode_info;
  DexesStructure m_dexes_structure;

  std::vector<DexType*> m_end_markers;
  std::vector<DexType*> m_scroll_markers;
};

} // namespace interdex
