/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional_io.hpp>

#include "DexTypeEnvironment.h"
#include "HashedAbstractPartition.h"
#include "InstructionAnalyzer.h"

namespace type_analyzer {

namespace global {

class GlobalTypeAnalyzer;

} // namespace global

using DexTypeFieldPartition =
    sparta::HashedAbstractPartition<const DexField*, DexTypeDomain>;

using DexTypeMethodPartition =
    sparta::HashedAbstractPartition<const DexMethod*, DexTypeDomain>;

class WholeProgramState {
 public:
  // By default, the field and method partitions are initialized to Bottom.
  WholeProgramState() = default;

  WholeProgramState(const Scope&,
                    const global::GlobalTypeAnalyzer&,
                    const std::unordered_set<DexMethod*>& non_true_virtuals);

  void set_to_top() {
    m_field_partition.set_to_top();
    m_method_partition.set_to_top();
  }

  bool leq(const WholeProgramState& other) const {
    return m_field_partition.leq(other.m_field_partition) &&
           m_method_partition.leq(other.m_method_partition);
  }

  /*
   * Returns our best approximation of the field type.
   *
   * It will never return Bottom.
   */
  DexTypeDomain get_field_type(const DexField* field) const {
    auto domain = m_field_partition.get(field);
    if (domain.is_bottom()) {
      return DexTypeDomain::top();
    }
    return domain;
  }

  /*
   * Returns our best static approximation of the return type.
   *
   * This may return Bottom to indicate that a method never returns (i.e. it
   * throws or loops indefinitely).
   */
  DexTypeDomain get_return_type(const DexMethod* method) const {
    if (!m_known_methods.count(method)) {
      return DexTypeDomain::top();
    }
    return m_method_partition.get(method);
  }

 private:
  void collect(const Scope& scope, const global::GlobalTypeAnalyzer&);

  void collect_field_types(
      const IRInstruction* insn,
      const DexTypeEnvironment& env,
      ConcurrentMap<const DexField*, std::vector<DexTypeDomain>>* field_tmp);

  void collect_return_types(
      const IRInstruction* insn,
      const DexTypeEnvironment& env,
      const DexMethod* method,
      ConcurrentMap<const DexMethod*, std::vector<DexTypeDomain>>* method_tmp);

  // Unknown methods will be treated as containing / returning Top.
  std::unordered_set<const DexMethod*> m_known_methods;

  DexTypeFieldPartition m_field_partition;
  DexTypeMethodPartition m_method_partition;
};

class WholeProgramAwareAnalyzer final
    : public InstructionAnalyzerBase<WholeProgramAwareAnalyzer,
                                     DexTypeEnvironment,
                                     const WholeProgramState*> {

 public:
  static bool analyze_invoke(const WholeProgramState* whole_program_state,
                             const IRInstruction* insn,
                             DexTypeEnvironment* env);
};
} // namespace type_analyzer
