/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "AssetManager.h"
#include "CrossDexRefMinimizer.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "DexUtil.h"
#include "InitClassesWithSideEffects.h"
#include "InterDexPassPlugin.h"
#include "MixedModeInfo.h"

class XStoreRefs;

namespace interdex {

constexpr size_t MAX_DEX_NUM = 99;

bool is_canary(DexClass* clazz);

DexClass* create_canary(int dexnum, const DexString* store_name = nullptr);

class InterDex {
 private:
  struct EmittingState {
    DexClassesVector outdex;
    std::vector<std::tuple<std::string, DexInfo>> dex_infos;
    DexesStructure dexes_structure;
  };

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
           bool fill_last_coldstart_dex,
           const cross_dex_ref_minimizer::CrossDexRefMinimizerConfig&
               cross_dex_refs_config,
           const ReserveRefsInfo& reserve_refs,
           const XStoreRefs* xstore_refs,
           int min_sdk,
           std::vector<std::string> methods_for_canary_clinit_reference,
           const init_classes::InitClassesWithSideEffects&
               init_classes_with_side_effects,
           bool transitively_close_interdex_order,
           int64_t minimize_cross_dex_refs_explore_alternatives)
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
        m_fill_last_coldstart_dex(fill_last_coldstart_dex),
        m_emitting_scroll_set(false),
        m_emitting_bg_set(false),
        m_emitted_bg_set(false),
        m_emitting_extended(false),
        m_cross_dex_ref_minimizer(cross_dex_refs_config),
        m_original_scope(original_scope),
        m_scope(build_class_scope(m_dexen)),
        m_xstore_refs(xstore_refs),
        m_methods_for_canary_clinit_reference(
            std::move(methods_for_canary_clinit_reference)),
        m_transitively_close_interdex_order(transitively_close_interdex_order),
        m_minimize_cross_dex_refs_explore_alternatives(
            minimize_cross_dex_refs_explore_alternatives) {
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

 private:
  void run_in_force_single_dex_mode();
  bool should_skip_class_due_to_plugin(DexClass* clazz) const;

  struct EmitResult {
    bool emitted{false};
    bool overflowed{false};

    operator bool() const { return emitted; }
  };

  struct FlushOutDexResult {
    size_t dex_count;
    bool primary_or_betamap_ordered;
  };

  EmitResult emit_class(
      EmittingState& emitting_state,
      DexInfo& dex_info,
      DexClass* clazz,
      bool check_if_skip,
      bool perf_sensitive,
      DexClass** canary_cls,
      std::optional<FlushOutDexResult>* opt_fodr = nullptr) const;
  void emit_primary_dex(
      const DexClasses& primary_dex,
      const std::vector<DexType*>& interdex_order,
      const std::unordered_set<DexClass*>& unreferenced_classes);
  void emit_interdex_classes(
      DexInfo& dex_info,
      const std::vector<DexType*>& interdex_types,
      const std::unordered_set<DexClass*>& unreferenced_classes,
      DexClass** canary_cls);
  void init_cross_dex_ref_minimizer();
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

  void set_clinit_methods_if_needed(DexClass* cls) const;

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
  bool m_emit_canaries;
  bool m_minimize_cross_dex_refs;
  bool m_fill_last_coldstart_dex;

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
  std::vector<std::string> m_methods_for_canary_clinit_reference;

  size_t m_transitive_closure_added{0};
  size_t m_transitive_closure_moved{0};
  const bool m_transitively_close_interdex_order;
  const int64_t m_minimize_cross_dex_refs_explore_alternatives;
};

} // namespace interdex
