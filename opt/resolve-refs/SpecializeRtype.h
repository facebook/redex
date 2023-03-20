/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Resolver.h"

class PassManager;
class DexTypeDomain;

namespace method_override_graph {
class Graph;
} // namespace method_override_graph

namespace resolve_refs {

using MethodToInferredReturnType =
    std::map<DexMethod*, const DexType*, dexmethods_comparator>;

struct RtypeStats {
  size_t num_rtype_specialized = 0;
  size_t num_rtype_specialized_direct = 0;
  size_t num_rtype_specialized_virtual_1 = 0;
  size_t num_rtype_specialized_virtual_1p = 0;
  size_t num_rtype_specialized_virtual_10p = 0;
  size_t num_rtype_specialized_virtual_100p = 0;

  void print(PassManager* mgr);
  RtypeStats& operator+=(const RtypeStats& that);
};

class RtypeCandidates final {
 public:
  void collect_inferred_rtype(const DexMethod* meth,
                              const DexTypeDomain& inferred_rtype,
                              DexTypeDomain& curr_rtype);
  void collect_specializable_rtype(DexMethod* meth,
                                   const DexTypeDomain& rtype_domain);
  const MethodToInferredReturnType& get_candidates() {
    return m_rtype_candidates;
  }
  RtypeCandidates& operator+=(const RtypeCandidates& that) {
    m_rtype_candidates.insert(that.m_rtype_candidates.begin(),
                              that.m_rtype_candidates.end());
    return *this;
  }

 private:
  MethodToInferredReturnType m_rtype_candidates;
};

class RtypeSpecialization final {
 public:
  explicit RtypeSpecialization(const MethodToInferredReturnType& candidates)
      : m_candidates(candidates) {}

  RtypeStats specialize_rtypes(const Scope& scope) const;

 private:
  const MethodToInferredReturnType m_candidates;

  void specialize_true_virtuals(
      const method_override_graph::Graph& override_graph,
      DexMethod* meth,
      const DexType* better_rtype,
      MethodSet& specialized,
      RtypeStats& stats) const;
  bool shares_identical_rtype_candidate(DexMethod* meth,
                                        const DexType* better_rtype) const;
};

} // namespace resolve_refs
