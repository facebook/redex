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

  bool not_zero() const {
    return num_patched_parameters != 0 || num_patched_fields_and_methods != 0;
  }
};

struct PatcherStats {
  Stats fix_kt_enum_ctor_param;
  Stats patch_lambdas;
  Stats patch_parameters_and_returns;
  Stats patch_synth_cls_fields_from_ctor_param;
  Stats patch_enclosing_lambda_fields;
  Stats patch_ctor_params_from_synth_cls_fields;
  Stats patch_chained_getters;

  PatcherStats() = default;

  PatcherStats& operator+=(const PatcherStats& other) {
    fix_kt_enum_ctor_param += other.fix_kt_enum_ctor_param;
    patch_lambdas += other.patch_lambdas;
    patch_parameters_and_returns += other.patch_parameters_and_returns;
    patch_synth_cls_fields_from_ctor_param +=
        other.patch_synth_cls_fields_from_ctor_param;
    patch_enclosing_lambda_fields += other.patch_enclosing_lambda_fields;
    patch_ctor_params_from_synth_cls_fields +=
        other.patch_ctor_params_from_synth_cls_fields;
    patch_chained_getters += other.patch_chained_getters;
    return *this;
  }
};

struct ParamCandidate {
  DexMethod* method;
  src_index_t index;

  ParamCandidate(DexMethod* method, src_index_t src_index)
      : method(method), index(src_index) {}
};

inline size_t hash_value(ParamCandidate pc) {
  return (reinterpret_cast<size_t>(pc.method)) ^ static_cast<size_t>(pc.index);
}

inline bool operator==(const ParamCandidate& a, const ParamCandidate& b) {
  return a.method == b.method && a.index == b.index;
}

inline bool compare_param_candidate(const ParamCandidate& l,
                                    const ParamCandidate& r) {
  if (l.method == r.method) {
    return l.index < r.index;
  }
  return compare_dexmethods(l.method, r.method);
}

class PatchingCandidates {

 public:
  void add_field_candidate(DexField* field, const TypedefAnnoType* anno) {
    m_field_candidates.get_or_emplace_and_assert_equal(
        field, const_cast<TypedefAnnoType*>(anno));
  }
  void add_method_candidate(DexMethod* method, const TypedefAnnoType* anno) {
    m_method_candidates.get_or_emplace_and_assert_equal(
        method, const_cast<TypedefAnnoType*>(anno));
  }
  void add_param_candidate(DexMethod* method,
                           const TypedefAnnoType* anno,
                           src_index_t index) {
    m_param_candidates.get_or_emplace_and_assert_equal(
        ParamCandidate(method, index), const_cast<TypedefAnnoType*>(anno));
  }
  size_t candidates_size() const {
    return m_field_candidates.size() + m_method_candidates.size() +
           m_param_candidates.size();
  }
  void apply_patching(Stats& class_stats);

 private:
  InsertOnlyConcurrentMap<DexField*, TypedefAnnoType*> m_field_candidates;
  InsertOnlyConcurrentMap<DexMethod*, TypedefAnnoType*> m_method_candidates;
  InsertOnlyConcurrentMap<ParamCandidate,
                          TypedefAnnoType*,
                          boost::hash<ParamCandidate>>
      m_param_candidates;
};

class TypedefAnnoPatcher {
 public:
  explicit TypedefAnnoPatcher(
      const TypedefAnnoCheckerPass::Config& config,
      const method_override_graph::Graph& method_override_graph)
      : m_method_override_graph(method_override_graph),
        m_max_iteration(config.max_patcher_iteration) {
    m_typedef_annos.insert(config.int_typedef);
    m_typedef_annos.insert(config.str_typedef);
  }

  void run(const Scope& scope);

  void print_stats(PassManager& mgr);

 private:
  void collect_overriding_method_candidates(DexMethod* m,
                                            PatchingCandidates& candidates);

  void collect_param_candidates(DexMethod* method,
                                PatchingCandidates& candidates);

  void collect_return_candidates(DexMethod* method,
                                 PatchingCandidates& candidates);

  void patch_enclosing_lambda_fields(const DexClass* cls, Stats& class_stats);

  void patch_synth_cls_fields_from_ctor_param(DexMethod* ctor,
                                              Stats& class_stats,
                                              PatchingCandidates& candidates);

  void patch_lambdas(DexMethod* method,
                     std::vector<const DexField*>* patched_fields,
                     PatchingCandidates& candidates);

  void patch_ctor_params_from_synth_cls_fields(DexClass* cls,
                                               Stats& class_stats);

  void fix_kt_enum_ctor_param(const DexClass* cls, Stats& class_stats);

  void populate_chained_getters(DexClass* cls);
  void patch_chained_getters(PatchingCandidates& candidates);

  UnorderedSet<const TypedefAnnoType*> m_typedef_annos;
  const method_override_graph::Graph& m_method_override_graph;
  const size_t m_max_iteration;
  ConcurrentMap<std::string, std::vector<const DexField*>> m_lambda_anno_map;
  InsertOnlyConcurrentSet<std::string_view> m_patched_returns;
  InsertOnlyConcurrentSet<DexClass*> m_chained_getters;

  PatcherStats m_patcher_stats;

  std::mutex m_anno_patching_mutex;
};
