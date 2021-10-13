/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "AssetManager.h"
#include "CrossDexRefMinimizer.h"
#include "CrossDexRelocator.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "InterDexPassPlugin.h"
#include "MixedModeInfo.h"

class XStoreRefs;

namespace interdex {

bool is_canary(DexClass* clazz);

class InterDex {
 public:
  InterDex(const Scope& original_scope,
           const DexClassesVector& dexen,
           AssetManager& asset_manager,
           ConfigFiles& conf,
           std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
           int64_t linear_alloc_limit,
           bool static_prune_classes,
           bool normal_primary_dex,
           bool keep_primary_order,
           bool force_single_dex,
           bool emit_canaries,
           bool minimize_cross_dex_refs,
           const cross_dex_ref_minimizer::CrossDexRefMinimizerConfig&
               cross_dex_refs_config,
           const CrossDexRelocatorConfig& cross_dex_relocator_config,
           size_t reserve_frefs,
           size_t reserve_trefs,
           size_t reserve_mrefs,
           const XStoreRefs* xstore_refs,
           int min_sdk,
           bool sort_remaining_classes,
           std::string method_for_canary_clinit_reference)
      : m_dexen(dexen),
        m_asset_manager(asset_manager),
        m_conf(conf),
        m_plugins(plugins),
        m_static_prune_classes(static_prune_classes),
        m_normal_primary_dex(normal_primary_dex),
        m_keep_primary_order(keep_primary_order),
        m_force_single_dex(force_single_dex),
        m_emit_canaries(emit_canaries),
        m_minimize_cross_dex_refs(minimize_cross_dex_refs),
        m_emitting_scroll_set(false),
        m_emitting_bg_set(false),
        m_emitted_bg_set(false),
        m_emitting_extended(false),
        m_cross_dex_ref_minimizer(cross_dex_refs_config),
        m_cross_dex_relocator_config(cross_dex_relocator_config),
        m_original_scope(original_scope),
        m_scope(build_class_scope(m_dexen)),
        m_xstore_refs(xstore_refs),
        m_sort_remaining_classes(sort_remaining_classes),
        m_method_for_canary_clinit_reference(
            std::move(method_for_canary_clinit_reference)) {
    m_dexes_structure.set_linear_alloc_limit(linear_alloc_limit);
    m_dexes_structure.set_reserve_frefs(reserve_frefs);
    m_dexes_structure.set_reserve_trefs(reserve_trefs);
    m_dexes_structure.set_reserve_mrefs(reserve_mrefs);
    m_dexes_structure.set_min_sdk(min_sdk);

    load_interdex_types();
  }

  ~InterDex() { delete m_cross_dex_relocator; }

  size_t get_num_cold_start_set_dexes() const {
    return m_dexes_structure.get_num_coldstart_dexes();
  }

  size_t get_num_scroll_dexes() const {
    return m_dexes_structure.get_num_scroll_dexes();
  }

  const cross_dex_ref_minimizer::CrossDexRefMinimizerStats&
  get_cross_dex_ref_minimizer_stats() const {
    return m_cross_dex_ref_minimizer.stats();
  }

  CrossDexRelocatorStats get_cross_dex_relocator_stats() const {
    if (m_cross_dex_relocator != nullptr) {
      return m_cross_dex_relocator->stats();
    }

    return CrossDexRelocatorStats();
  }

  /**
   * Only call this if you know what you are doing.
   * This will leave the current instance is in an unusable state.
   */
  DexClassesVector take_outdex() { return std::move(m_outdex); }

  void run();
  void run_on_nonroot_store();
  void add_dexes_from_store(const DexStore& store);
  void cleanup(const Scope& final_scope);

  const std::vector<DexType*>& get_interdex_types() const {
    return m_interdex_types;
  }

  size_t get_current_classes_when_emitting_remaining() const {
    return m_current_classes_when_emitting_remaining;
  }

 private:
  void run_in_force_single_dex_mode();
  bool should_not_relocate_methods_of_class(const DexClass* clazz);
  void add_to_scope(DexClass* cls);
  bool should_skip_class_due_to_plugin(DexClass* clazz);
  bool emit_class(DexInfo& dex_info,
                  DexClass* clazz,
                  bool check_if_skip,
                  bool perf_sensitive,
                  std::vector<DexClass*>* erased_classes = nullptr);
  void emit_primary_dex(
      const DexClasses& primary_dex,
      const std::vector<DexType*>& interdex_order,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void emit_interdex_classes(
      DexInfo& dex_info,
      const std::vector<DexType*>& interdex_types,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void init_cross_dex_ref_minimizer_and_relocate_methods();
  void emit_remaining_classes(DexInfo& dex_info);
  void flush_out_dex(DexInfo& dex_info);

  void set_clinit_method_if_needed(DexClass* cls);

  /**
   * Stores in m_interdex_order a list of coldstart types. It will only contain:
   * * classes that still exist in the current scope
   * * + a "fake" type for each of the class markers (ex: DexEndMarker etc)
   */
  void load_interdex_types();

  /**
   * Makes sure that classes in the dex end up in the interdex list.
   * For the classes that aren't already in the list, it adds them at
   * the beginning.
   */
  void update_interdexorder(const DexClasses& dex,
                            std::vector<DexType*>* interdex_types);

  const DexClassesVector& m_dexen;
  DexClassesVector m_outdex;
  AssetManager& m_asset_manager;
  ConfigFiles& m_conf;
  std::vector<std::unique_ptr<InterDexPassPlugin>>& m_plugins;
  // TODO: Encapsulate (primary|all) dex flags under one config.
  bool m_static_prune_classes;
  bool m_normal_primary_dex;
  bool m_keep_primary_order;
  bool m_force_single_dex;
  bool m_emit_canaries;
  bool m_minimize_cross_dex_refs;

  bool m_emitting_scroll_set;
  bool m_emitting_bg_set;
  bool m_emitted_bg_set;
  bool m_emitting_extended;

  std::vector<std::tuple<std::string, DexInfo>> m_dex_infos;
  DexesStructure m_dexes_structure;

  std::vector<DexType*> m_end_markers;
  std::vector<DexType*> m_scroll_markers;

  cross_dex_ref_minimizer::CrossDexRefMinimizer m_cross_dex_ref_minimizer;
  const CrossDexRelocatorConfig m_cross_dex_relocator_config;
  const Scope& m_original_scope;
  CrossDexRelocator* m_cross_dex_relocator{nullptr};
  Scope m_scope;
  std::vector<DexType*> m_interdex_types;
  const XStoreRefs* m_xstore_refs;
  bool m_sort_remaining_classes;
  size_t m_current_classes_when_emitting_remaining{0};
  std::string m_method_for_canary_clinit_reference;
};

} // namespace interdex
