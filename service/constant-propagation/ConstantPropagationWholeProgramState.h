/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "CallGraph.h"
#include "ConstantEnvironment.h"
#include "HashedAbstractPartition.h"
#include "InstructionAnalyzer.h"

namespace constant_propagation {

namespace interprocedural {

class FixpointIterator;

} // namespace interprocedural

using ConstantStaticFieldPartition =
    HashedAbstractPartition<const DexField*, ConstantValue>;

using ConstantMethodPartition =
    HashedAbstractPartition<const DexMethod*, ConstantValue>;

/*
 * This class contains flow-insensitive information about fields and method
 * return values, i.e. it can tells us if a field or a return value is constant
 * throughout the entire program.
 *
 * It exposes a read-only interface to emphasize that it is never written to as
 * part of the inter/intra-procedural fixpoint iteration process. Instead, it
 * takes the results of a completed fixpoint iteration and extracts the
 * constant values.
 */
class WholeProgramState {
 public:
  WholeProgramState() {
    m_field_partition.set_to_top();
    m_method_partition.set_to_top();
  }

  WholeProgramState(const Scope&, const interprocedural::FixpointIterator&);
  bool leq(const WholeProgramState& other) const {
    return m_field_partition.leq(other.m_field_partition) &&
           m_method_partition.leq(other.m_method_partition);
  }

  /*
   * Returns our best static approximation of the field value.
   *
   * This method can be passed both static and non-static fields, but as of now
   * it will always return Top for non-static fields.
   *
   * It will never return Bottom.
   */
  const ConstantValue get_field_value(const DexField* field) const {
    if (!m_known_fields.count(field)) {
      return SignedConstantDomain::top();
    }
    return m_field_partition.get(field);
  }

  /*
   * Returns our best static approximation of the return value.
   *
   * This may return Bottom to indicate that a method never returns (i.e. it
   * throws or loops indefinitely).
   */
  const ConstantValue get_return_value(const DexMethod* method) const {
    if (!m_known_methods.count(method)) {
      return SignedConstantDomain::top();
    }
    return m_method_partition.get(method);
  }

  const ConstantStaticFieldPartition& get_field_partition() const {
    return m_field_partition;
  }

  const ConstantMethodPartition& get_method_partition() const {
    return m_method_partition;
  }

 private:
  void set_fields_with_encoded_values(const Scope&);

  void collect(const Scope& scope,
               const interprocedural::FixpointIterator& fp_iter);

  void collect_field_values(const IRInstruction* insn,
                            const ConstantEnvironment& env,
                            const DexType* clinit_cls);

  void collect_return_values(const IRInstruction* insn,
                             const ConstantEnvironment& env,
                             const DexMethod* method);

  // Unknown fields and methods will be treated as containing / returning Top.
  std::unordered_set<const DexField*> m_known_fields;
  std::unordered_set<const DexMethod*> m_known_methods;

  // A partition represents a set of execution paths that reach certain control
  // points (like invoke/return statements). The abstract information
  // associated to these terminal control points denotes the union of all
  // possible concrete states over the corresponding execution paths. In
  // contrast, an abstract environment represents the intersection of a
  // collection of abstract data over some execution paths. See proposition 12
  // of the following paper for more detail on partitions:
  // https://cs.nyu.edu/~pcousot/publications.www/CousotCousot-JLP-v2-n4-p511--547-1992.pdf
  //
  // One can think of the bindings in these DexMember-labeled Partitions as
  // modeling the state of the result register after the execution of any sget
  // or invoke instruction that references the DexMember. Since each label
  // represents a subset of control points, we should use a Partition rather
  // than an Environment here.
  //
  // This is particularly relevant for method return values -- a method can
  // "return" Bottom by throwing or never terminating, in which case we want to
  // bind it to Bottom here, but doing so in an Environment would set the whole
  // Environment to Bottom.
  ConstantStaticFieldPartition m_field_partition;
  ConstantMethodPartition m_method_partition;
};

/*
 * Incorporate information about the values of static fields and the return
 * values of other methods in the local analysis of a given method.
 */
class WholeProgramAwareAnalyzer final
    : public InstructionAnalyzerBase<WholeProgramAwareAnalyzer,
                                     ConstantEnvironment,
                                     const WholeProgramState*> {
 public:
  static bool analyze_sget(const WholeProgramState* whole_program_state,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_invoke(const WholeProgramState* whole_program_state,
                             const IRInstruction* insn,
                             ConstantEnvironment* env);
};

} // namespace constant_propagation
