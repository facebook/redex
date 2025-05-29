/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "AssetManager.h"
#include "BaselineProfileConfig.h"
#include "CrossDexRefMinimizer.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexStoreUtil.h"
#include "DexStructure.h"
#include "DexUtil.h"
#include "InitClassesWithSideEffects.h"
#include "InterDexPassPlugin.h"
#include "MixedModeInfo.h"

class XStoreRefs;

namespace interdex {

constexpr size_t MAX_DEX_NUM = 99;

class InterDex {
 private:
  struct EmittingState {
    DexClassesVector outdex;
    std::vector<std::tuple<std::string, DexInfo>> dex_infos;
    DexesStructure dexes_structure;
  };

 public:
  InterDex(
      const Scope& original_scope,
      const DexClassesVector& dexen,
      AssetManager& asset_manager,
      ConfigFiles& conf,
      std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
      int64_t linear_alloc_limit,
      bool static_prune_classes,
      bool normal_primary_dex,
      bool keep_primary_order,
      bool force_single_dex,
      bool order_interdex,
      bool emit_canaries,
      bool minimize_cross_dex_refs,
      bool fill_last_coldstart_dex,
      bool reorder_dynamically_dead_classes,
      const cross_dex_ref_minimizer::CrossDexRefMinimizerConfig&
          cross_dex_refs_config,
      const ReserveRefsInfo& reserve_refs,
      const XStoreRefs* xstore_refs,
      int min_sdk,
      const init_classes::InitClassesWithSideEffects&
          init_classes_with_side_effects,
      bool transitively_close_interdex_order,
      int64_t minimize_cross_dex_refs_explore_alternatives,
      ClassReferencesCache& class_references_cache,
      bool exclude_baseline_profile_classes,
      const baseline_profiles::BaselineProfileConfig& baseline_profile_config,
      bool move_coldstart_classes,
      size_t min_betamap_move_threshold,
      size_t max_betamap_move_threshold,
      int64_t stable_partitions,
      bool is_root_store = true)
      : m_dexen(dexen),
        m_asset_manager(asset_manager),
        m_conf(conf),
        m_plugins(plugins),
        m_static_prune_classes(static_prune_classes),
        m_normal_primary_dex(normal_primary_dex),
        m_keep_primary_order(keep_primary_order),
        m_force_single_dex(force_single_dex),
        m_order_interdex(order_interdex),
        m_emit_canaries(emit_canaries),
        m_minimize_cross_dex_refs(minimize_cross_dex_refs),
        m_fill_last_coldstart_dex(fill_last_coldstart_dex),
        m_reorder_dynamically_dead_classes(reorder_dynamically_dead_classes),
        m_emitting_dynamically_dead_dex(false),
        m_emitting_scroll_set(false),
        m_emitting_bg_set(false),
        m_emitted_bg_set(false),
        m_emitting_extended(false),
        m_cross_dex_ref_minimizer(cross_dex_refs_config,
                                  class_references_cache),
        m_original_scope(original_scope),
        m_scope(build_class_scope(m_dexen)),
        m_xstore_refs(xstore_refs),
        m_transitively_close_interdex_order(transitively_close_interdex_order),
        m_minimize_cross_dex_refs_explore_alternatives(
            minimize_cross_dex_refs_explore_alternatives),
        m_class_references_cache(class_references_cache),
        m_exclude_baseline_profile_classes(exclude_baseline_profile_classes),
        m_baseline_profile_config(baseline_profile_config),
        m_move_coldstart_classes(move_coldstart_classes),
        m_min_betamap_move_threshold(min_betamap_move_threshold),
        m_max_betamap_move_threshold(max_betamap_move_threshold),
        m_stable_partitions(stable_partitions),
        m_is_root_store(is_root_store) {
    m_emitting_state.dexes_structure.set_linear_alloc_limit(linear_alloc_limit);
    m_emitting_state.dexes_structure.set_reserve_frefs(reserve_refs.frefs);
    m_emitting_state.dexes_structure.set_reserve_trefs(reserve_refs.trefs);
    m_emitting_state.dexes_structure.set_reserve_mrefs(reserve_refs.mrefs);
    m_emitting_state.dexes_structure.set_min_sdk(min_sdk);
    m_emitting_state.dexes_structure.set_init_classes_with_side_effects(
        &init_classes_with_side_effects);

    load_interdex_types();
  }

  size_t get_num_cold_start_set_dexes() const {
    return m_emitting_state.dexes_structure.get_num_coldstart_dexes();
  }

  size_t get_num_scroll_dexes() const {
    return m_emitting_state.dexes_structure.get_num_scroll_dexes();
  }

  const cross_dex_ref_minimizer::CrossDexRefMinimizerStats&
  get_cross_dex_ref_minimizer_stats() const {
    return m_cross_dex_ref_minimizer.stats();
  }

  /**
   * Only call this if you know what you are doing.
   * This will leave the current instance is in an unusable state.
   */
  DexClassesVector take_outdex() { return std::move(m_emitting_state.outdex); }

  void run();
  void run_on_nonroot_store();
  void add_dexes_from_store(const DexStore& store);

  const std::vector<DexType*>& get_interdex_types() const {
    return m_interdex_types;
  }

  size_t get_current_classes_when_emitting_remaining() const {
    return m_current_classes_when_emitting_remaining;
  }

  size_t get_transitive_closure_added() const {
    return m_transitive_closure_added;
  }

  size_t get_transitive_closure_moved() const {
    return m_transitive_closure_moved;
  }

  const std::vector<DexInfo>& get_dex_info() const {
    return m_emitting_state.dexes_structure.get_dex_info();
  }

  const OverflowStats& get_overflow_stats() const {
    return m_emitting_state.dexes_structure.get_overflow_stats();
  }

 private:
  void run_in_force_single_dex_mode();
  bool should_skip_class_due_to_dynamically_dead(DexClass* clazz) const;
  bool should_skip_class_due_to_plugin(DexClass* clazz) const;

  struct EmitResult {
    bool emitted{false};
    bool overflowed{false};
  };

  struct FlushOutDexResult {
    size_t dex_count;
    bool primary_or_betamap_ordered;
  };

  EmitResult emit_class(EmittingState& emitting_state,
                        DexInfo& dex_info,
                        DexClass* clazz,
                        bool check_if_skip,
                        bool perf_sensitive,
                        DexClass** canary_cls,
                        std::optional<FlushOutDexResult>* opt_fodr = nullptr,
                        bool skip_dynamically_dead = false) const;
  void emit_primary_dex(const DexClasses& primary_dex,
                        const std::vector<DexType*>& interdex_order,
                        const UnorderedSet<DexClass*>& unreferenced_classes);
  void emit_interdex_classes(
      DexInfo& dex_info,
      const std::vector<DexType*>& interdex_types,
      const UnorderedSet<DexClass*>& unreferenced_classes,
      DexClass** canary_cls);
  void init_cross_dex_ref_minimizer();
  std::vector<DexClasses> get_stable_partitions();
  void emit_remaining_classes(DexInfo& dex_info, DexClass** canary_cls);
  void emit_remaining_classes_legacy(DexInfo& dex_info, DexClass** canary_cls);
  void emit_remaining_classes_exploring_alternatives(DexInfo& dex_info,
                                                     DexClass** canary_cls);
  DexClass* get_canary_cls(EmittingState& emitting_state,
                           DexInfo& dex_info) const;

  FlushOutDexResult flush_out_dex(EmittingState& emitting_state,
                                  DexInfo& dex_info,
                                  DexClass* canary_cls) const;
  void post_process_dex(EmittingState& emitting_state,
                        const FlushOutDexResult&) const;

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

  /*
   * Removes baseline profile classes (based on config specified in refig) from
   * m_interdex_types. Still sets them to perf-sensitive as BETAMAP_ORDERED.
   */
  void exclude_baseline_profile_classes();

  bool is_baseline_profile_class(DexType* dex_type) const {
    // Assumes m_baseline_profile_classes is set
    return m_baseline_profile_classes->find(dex_type) !=
           m_baseline_profile_classes->end();
  }

  void initialize_baseline_profile_classes();

  void get_movable_coldstart_classes(
      const std::vector<DexType*>& interdex_types,
      UnorderedMap<const DexClass*, std::string>& move_coldstart_classes);

  EmittingState m_emitting_state;

  const DexClassesVector& m_dexen;
  AssetManager& m_asset_manager;
  ConfigFiles& m_conf;
  std::vector<std::unique_ptr<InterDexPassPlugin>>& m_plugins;
  // TODO: Encapsulate (primary|all) dex flags under one config.
  bool m_static_prune_classes;
  bool m_normal_primary_dex;
  bool m_keep_primary_order;
  bool m_force_single_dex;
  bool m_order_interdex;
  bool m_emit_canaries;
  bool m_minimize_cross_dex_refs;
  bool m_fill_last_coldstart_dex;
  bool m_reorder_dynamically_dead_classes;
  // True if dynamically_dead_classes are emitting.
  bool m_emitting_dynamically_dead_dex;

  UnorderedMap<const DexClass*, size_t> m_interdex_order;
  bool m_emitting_scroll_set;
  bool m_emitting_bg_set;
  bool m_emitted_bg_set;
  bool m_emitting_extended;

  std::vector<DexType*> m_end_markers;
  std::vector<DexType*> m_scroll_markers;

  cross_dex_ref_minimizer::CrossDexRefMinimizer m_cross_dex_ref_minimizer;
  const Scope& m_original_scope;
  Scope m_scope;
  std::vector<DexType*> m_interdex_types;
  const XStoreRefs* m_xstore_refs;
  size_t m_current_classes_when_emitting_remaining{0};

  size_t m_transitive_closure_added{0};
  size_t m_transitive_closure_moved{0};
  const bool m_transitively_close_interdex_order;
  const int64_t m_minimize_cross_dex_refs_explore_alternatives;

  ClassReferencesCache& m_class_references_cache;

  bool m_exclude_baseline_profile_classes;
  const baseline_profiles::BaselineProfileConfig& m_baseline_profile_config;
  std::optional<UnorderedSet<DexType*>> m_baseline_profile_classes;
  bool m_move_coldstart_classes;
  size_t m_min_betamap_move_threshold;
  size_t m_max_betamap_move_threshold;

  const uint64_t m_stable_partitions;
  const bool m_is_root_store;
};

} // namespace interdex
