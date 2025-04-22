/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

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

using TypedefAnnoType = DexType;

struct Stats {
  size_t num_patched_parameters{0};
  size_t num_patched_fields_and_methods{0};
  Stats() = default;

  Stats& operator+=(const Stats& other) {
    num_patched_parameters += other.num_patched_parameters;
    num_patched_fields_and_methods += other.num_patched_fields_and_methods;
    return *this;
  }
};

struct PatcherStats {
  Stats fix_kt_enum_ctor_param{};
  Stats patch_lambdas{};
  Stats patch_parameters_and_returns{};
  Stats patch_synth_methods_overriding_annotated_methods{};
  Stats patch_synth_cls_fields_from_ctor_param{};
  Stats patch_enclosing_lambda_fields{};
  Stats patch_ctor_params_from_synth_cls_fields{};
  Stats patch_chained_getters{};

  PatcherStats() = default;

  PatcherStats& operator+=(const PatcherStats& other) {
    fix_kt_enum_ctor_param += other.fix_kt_enum_ctor_param;
    patch_lambdas += other.patch_lambdas;
    patch_parameters_and_returns += other.patch_parameters_and_returns;
    patch_synth_methods_overriding_annotated_methods +=
        other.patch_synth_methods_overriding_annotated_methods;
    patch_synth_cls_fields_from_ctor_param +=
        other.patch_synth_cls_fields_from_ctor_param;
    patch_enclosing_lambda_fields += other.patch_enclosing_lambda_fields;
    patch_ctor_params_from_synth_cls_fields +=
        other.patch_ctor_params_from_synth_cls_fields;
    patch_chained_getters += other.patch_chained_getters;
    return *this;
  }
};

class PatchingCandidates {

 public:
  void add_field_candidate(DexField* field, const TypedefAnnoType* anno) {
    m_field_candidates.insert_or_assign(
        std::make_pair(field, const_cast<TypedefAnnoType*>(anno)));
  }
  void add_method_candidate(DexMethod* method, const TypedefAnnoType* anno) {
    m_method_candidates.insert_or_assign(
        std::make_pair(method, const_cast<TypedefAnnoType*>(anno)));
  }
  void apply_patching(std::mutex& mutex, Stats& class_stats);

 private:
  ConcurrentMap<DexField*, TypedefAnnoType*> m_field_candidates;
  ConcurrentMap<DexMethod*, TypedefAnnoType*> m_method_candidates;
};

struct ParamCandidate {
  DexMethod* method;
  TypedefAnnoType* anno;
  src_index_t index;

  ParamCandidate(DexMethod* method,
                 const TypedefAnnoType* anno,
                 src_index_t src_index)
      : method(method),
        anno(const_cast<TypedefAnnoType*>(anno)),
        index(src_index) {}
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

  void print_stats(PassManager& mgr);

 private:
  bool patch_if_overriding_annotated_methods(DexMethod* m, Stats& class_stats);

  void patch_parameters_and_returns(
      DexMethod* method,
      Stats& class_stats,
      std::vector<ParamCandidate>* missing_param_annos = nullptr);

  void patch_enclosing_lambda_fields(const DexClass* cls, Stats& class_stats);

  void patch_synth_cls_fields_from_ctor_param(DexMethod* ctor,
                                              Stats& class_stats);

  void patch_lambdas(DexMethod* method,
                     std::vector<const DexField*>* patched_fields,
                     PatchingCandidates& candidates,
                     Stats& class_stats);

  void patch_ctor_params_from_synth_cls_fields(DexClass* cls,
                                               Stats& class_stats);

  void fix_kt_enum_ctor_param(const DexClass* cls, Stats& class_stats);

  void populate_chained_getters(DexClass* cls);
  void patch_chained_getters(Stats& class_stats);

  UnorderedSet<TypedefAnnoType*> m_typedef_annos;
  const method_override_graph::Graph& m_method_override_graph;
  ConcurrentMap<std::string, std::vector<const DexField*>> m_lambda_anno_map;
  InsertOnlyConcurrentSet<std::string_view> m_patched_returns;
  InsertOnlyConcurrentSet<DexClass*> m_chained_getters;

  PatcherStats m_patcher_stats;

  std::mutex m_anno_patching_mutex;
};
