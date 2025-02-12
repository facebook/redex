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

 private:
  bool patch_synth_methods_overriding_annotated_methods(DexMethod* m);

  void patch_parameters_and_returns(
      DexMethod* method,
      std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
          missing_param_annos = nullptr);

  void patch_enclosed_method(DexClass* cls);

  void patch_synth_cls_fields_from_ctor_param(DexMethod* ctor);

  void patch_lambdas(DexMethod* method,
                     std::vector<const DexField*>* patched_fields);

  void patch_ctor_params_from_synth_cls_fields(DexClass* cls);

  void populate_chained_getters(DexClass* cls);
  void patch_chained_getters();

  std::unordered_set<DexType*> m_typedef_annos;
  const method_override_graph::Graph& m_method_override_graph;
  InsertOnlyConcurrentMap<std::string, std::vector<const DexField*>>
      m_lambda_anno_map;
  InsertOnlyConcurrentSet<std::string_view> m_patched_returns;
  InsertOnlyConcurrentSet<DexClass*> m_chained_getters;
};
