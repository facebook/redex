/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "TypeInference.h"
#include "TypedefAnnoCheckerPass.h"

namespace typedef_anno {
bool is_not_str_nor_int(const type_inference::TypeEnvironment& env, reg_t reg);
} // namespace typedef_anno

struct PatcherStats {
  size_t num_patched_parameters{0};
  size_t num_patched_fields_and_methods{0};
  PatcherStats() = default;

  PatcherStats& operator+=(const PatcherStats& other) {
    num_patched_parameters += other.num_patched_parameters;
    num_patched_fields_and_methods += other.num_patched_fields_and_methods;
    return *this;
  }
};

class TypedefAnnoPatcher {
 public:
  explicit TypedefAnnoPatcher(
      const TypedefAnnoCheckerPass::Config& config,
      const method_override_graph::Graph& method_override_graph)
      : m_method_override_graph(method_override_graph) {
    m_typedef_annos.insert(config.int_typedef);
    m_typedef_annos.insert(config.str_typedef);
  }

  void run(const Scope& scope);

  PatcherStats get_patcher_stats() { return m_patcher_stats; }
  PatcherStats get_chained_patcher_stats() { return m_chained_patcher_stats; }
  PatcherStats get_chained_getter_patcher_stats() {
    return m_chained_getter_patcher_stats;
  }

 private:
  bool patch_synth_methods_overriding_annotated_methods(
      DexMethod* m, PatcherStats& class_stats);

  void patch_parameters_and_returns(
      DexMethod* method,
      PatcherStats& class_stats,
      std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
          missing_param_annos = nullptr);

  void patch_enclosed_method(DexClass* cls, PatcherStats& class_stats);

  void patch_synth_cls_fields_from_ctor_param(DexMethod* ctor,
                                              PatcherStats& class_stats);

  void patch_lambdas(DexMethod* method,
                     std::vector<const DexField*>* patched_fields,
                     PatcherStats& class_stats);

  void patch_ctor_params_from_synth_cls_fields(DexClass* cls,
                                               PatcherStats& class_stats);

  void populate_chained_getters(DexClass* cls);
  void patch_chained_getters(PatcherStats& class_stats);

  std::unordered_set<DexType*> m_typedef_annos;
  const method_override_graph::Graph& m_method_override_graph;
  InsertOnlyConcurrentMap<std::string, std::vector<const DexField*>>
      m_lambda_anno_map;
  InsertOnlyConcurrentSet<std::string_view> m_patched_returns;
  InsertOnlyConcurrentSet<DexClass*> m_chained_getters;

  PatcherStats m_patcher_stats;
  PatcherStats m_chained_patcher_stats;
  PatcherStats m_chained_getter_patcher_stats;
};
