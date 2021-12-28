/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CommonSubexpressionElimination.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationTransform.h"
#include "CopyPropagation.h"
#include "DedupBlocks.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IPConstantPropagationAnalysis.h"
#include "IRCode.h"
#include "InitClassesWithSideEffects.h"
#include "LocalDce.h"
#include "MethodProfiles.h"
#include "RandomForest.h"
#include "ShrinkerConfig.h"
#include "Timer.h"

class ScopedMetrics;

namespace shrinker {

class Shrinker {
 public:
  Shrinker(
      DexStoresVector& stores,
      const Scope& scope,
      const init_classes::InitClassesWithSideEffects&
          init_classes_with_side_effects,
      const ShrinkerConfig& config,
      int min_sdk,
      const std::unordered_set<DexMethodRef*>& configured_pure_methods = {},
      const std::unordered_set<const DexString*>&
          configured_finalish_field_names = {});

  constant_propagation::Transform::Stats constant_propagation(
      bool is_static,
      DexType* declaring_type,
      DexProto*,
      IRCode* code,
      const ConstantEnvironment&,
      const constant_propagation::Transform::Config& config);
  LocalDce::Stats local_dce(IRCode* code,
                            bool normalize_new_instances = true,
                            DexType* declaring_type = nullptr);
  copy_propagation_impl::Stats copy_propagation(DexMethod* method);

  void shrink_method(DexMethod* method);
  const constant_propagation::Transform::Stats& get_const_prop_stats() const {
    return m_const_prop_stats;
  }
  const cse_impl::Stats& get_cse_stats() const { return m_cse_stats; }
  const copy_propagation_impl::Stats& get_copy_prop_stats() const {
    return m_copy_prop_stats;
  }
  const LocalDce::Stats& get_local_dce_stats() const {
    return m_local_dce_stats;
  }
  const dedup_blocks_impl::Stats& get_dedup_blocks_stats() const {
    return m_dedup_blocks_stats;
  }
  size_t get_methods_shrunk() const { return m_methods_shrunk; }
  size_t get_methods_reg_alloced() const { return m_methods_reg_alloced; }

  bool enabled() const { return m_enabled; }

  const std::unordered_set<const DexField*>* get_finalizable_fields() const {
    return m_cse_shared_state ? &m_cse_shared_state->get_finalizable_fields()
                              : nullptr;
  }

  const XStoreRefs& get_xstores() const { return m_xstores; }

  double get_const_prop_seconds() const {
    return m_const_prop_timer.get_seconds();
  }
  double get_cse_seconds() const { return m_cse_timer.get_seconds(); }
  double get_copy_prop_seconds() const {
    return m_copy_prop_timer.get_seconds();
  }
  double get_local_dce_seconds() const {
    return m_local_dce_timer.get_seconds();
  }
  double get_dedup_blocks_seconds() const {
    return m_dedup_blocks_timer.get_seconds();
  }
  double get_reg_alloc_seconds() const {
    return m_reg_alloc_timer.get_seconds();
  }
  double get_fast_reg_alloc_seconds() const {
    return m_fast_reg_alloc_timer.get_seconds();
  }

  struct MethodContext {
    uint32_t m_regs{0};
    uint32_t m_insns{0};
    uint32_t m_blocks{0};
    uint32_t m_edges{0};
  };

  using ShrinkerForest = ::random_forest::Forest<const MethodContext&>;

  const std::unordered_set<DexMethodRef*>& get_pure_methods() const {
    return m_pure_methods;
  }

  constant_propagation::ImmutableAttributeAnalyzerState*
  get_immut_analyzer_state() {
    return &m_immut_analyzer_state;
  }

  void log_metrics(ScopedMetrics& sm) const;

  const init_classes::InitClassesWithSideEffects&
  get_init_classes_with_side_effects() const {
    return m_init_classes_with_side_effects;
  }

 private:
  ShrinkerForest m_forest;
  const XStoreRefs m_xstores;
  const ShrinkerConfig m_config;
  const int m_min_sdk;
  const bool m_enabled;
  std::unique_ptr<cse_impl::SharedState> m_cse_shared_state;

  const init_classes::InitClassesWithSideEffects&
      m_init_classes_with_side_effects;
  std::unordered_set<DexMethodRef*> m_pure_methods;
  std::unordered_set<const DexString*> m_finalish_field_names;

  constant_propagation::ImmutableAttributeAnalyzerState m_immut_analyzer_state;

  // THe mutex protects all other mutable (stats) fields.
  std::mutex m_stats_mutex;
  AccumulatingTimer m_const_prop_timer;
  constant_propagation::Transform::Stats m_const_prop_stats;
  AccumulatingTimer m_cse_timer;
  cse_impl::Stats m_cse_stats;
  AccumulatingTimer m_copy_prop_timer;
  copy_propagation_impl::Stats m_copy_prop_stats;
  AccumulatingTimer m_local_dce_timer;
  LocalDce::Stats m_local_dce_stats;
  AccumulatingTimer m_dedup_blocks_timer;
  dedup_blocks_impl::Stats m_dedup_blocks_stats;
  AccumulatingTimer m_reg_alloc_timer;
  AccumulatingTimer m_fast_reg_alloc_timer;
  size_t m_methods_shrunk{0};
  size_t m_methods_reg_alloced{0};
};

} // namespace shrinker
