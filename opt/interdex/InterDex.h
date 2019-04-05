/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ApkManager.h"
#include "CrossDexRefMinimizer.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "InterDexPassPlugin.h"
#include "MixedModeInfo.h"

namespace interdex {

struct InterDexStats {
  size_t classes_added_for_relocated_methods{0};
  size_t relocatable_methods{0};
  size_t relocated_methods{0};
};

class InterDex {
 public:
  InterDex(const DexClassesVector& dexen,
           ApkManager& apk_manager,
           ConfigFiles& cfg,
           std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
           int64_t linear_alloc_limit,
           int64_t type_refs_limit,
           bool static_prune_classes,
           bool normal_primary_dex,
           bool emit_scroll_set_marker,
           bool emit_canaries,
           bool minimize_cross_dex_refs,
           const CrossDexRefMinimizerConfig& minimize_cross_dex_refs_config,
           bool minimize_cross_dex_refs_relocate_methods,
           size_t relocated_methods_per_class,
           size_t reserve_mrefs)
      : m_dexen(dexen),
        m_apk_manager(apk_manager),
        m_cfg(cfg),
        m_plugins(plugins),
        m_static_prune_classes(static_prune_classes),
        m_normal_primary_dex(normal_primary_dex),
        m_emit_canaries(emit_canaries),
        m_minimize_cross_dex_refs(minimize_cross_dex_refs),
        m_minimize_cross_dex_refs_relocate_methods(
            minimize_cross_dex_refs_relocate_methods),
        m_relocated_methods_per_class(relocated_methods_per_class),
        m_cross_dex_ref_minimizer(minimize_cross_dex_refs_config) {
    m_dexes_structure.set_linear_alloc_limit(linear_alloc_limit);
    m_dexes_structure.set_type_refs_limit(type_refs_limit);
    m_dexes_structure.set_reserve_mrefs(reserve_mrefs);
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

  const CrossDexRefMinimizerStats& get_cross_dex_ref_minimizer_stats() const {
    return m_cross_dex_ref_minimizer.stats();
  }

  const InterDexStats& get_stats() const { return m_stats; }

  /**
   * Only call this if you know what you are doing.
   * This will leave the current instance is in an unusable state.
   */
  DexClassesVector take_outdex() { return std::move(m_outdex); }

  void run();
  void add_dexes_from_store(const DexStore& store);

 private:
  bool should_not_relocate_methods_of_class(const DexClass* clazz);
  void add_to_scope(DexClass* cls);
  bool should_skip_class_due_to_plugin(DexClass* clazz);
  bool should_skip_class_due_to_mixed_mode(const DexInfo& dex_info,
                                           DexClass* clazz);
  bool emit_class(const DexInfo& dex_info,
                  DexClass* clazz,
                  bool check_if_skip,
                  bool perf_sensitive,
                  std::vector<DexClass*>* erased_classes = nullptr);
  void emit_primary_dex(
      const DexClasses& primary_dex,
      const std::vector<DexType*>& interdex_order,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void emit_interdex_classes(
      const std::vector<DexType*>& interdex_types,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  struct RelocatedMethodInfo {
    DexMethod* method;
    DexClass* source_class;
    int api_level;
  };
  void init_cross_dex_ref_minimizer_and_relocate_methods(
      const Scope& scope,
      std::unordered_map<DexClass*, RelocatedMethodInfo>& relocated);
  void emit_remaining_classes(const Scope& scope);
  struct RelocatedTargetClassInfo {
    DexClass* cls;
    size_t size{0}; // number of methods
  };
  void re_relocate_method(
      DexClass* cls,
      const std::unordered_set<DexClass*>& classes_in_current_dex,
      const std::unordered_map<DexClass*, RelocatedMethodInfo>& relocated,
      std::unordered_map<int32_t, RelocatedTargetClassInfo>&
          relocated_target_classes);
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
  bool m_minimize_cross_dex_refs;
  bool m_minimize_cross_dex_refs_relocate_methods;
  size_t m_relocated_methods_per_class;

  MixedModeInfo m_mixed_mode_info;
  DexesStructure m_dexes_structure;

  std::vector<DexType*> m_end_markers;
  std::vector<DexType*> m_scroll_markers;

  CrossDexRefMinimizer m_cross_dex_ref_minimizer;
  InterDexStats m_stats;
};

} // namespace interdex
