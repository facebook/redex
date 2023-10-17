/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/HashedAbstractPartition.h>

#include "CallGraph.h"
#include "ConstantEnvironment.h"
#include "InstructionAnalyzer.h"

namespace constant_propagation {

enum class FieldType { INSTANCE, STATIC };

using EligibleIfields = std::unordered_set<DexField*>;

namespace interprocedural {

class FixpointIterator;

} // namespace interprocedural

using ConstantFieldPartition =
    sparta::HashedAbstractPartition<const DexField*, ConstantValue>;

using ConstantMethodPartition =
    sparta::HashedAbstractPartition<const DexMethod*, ConstantValue>;

/*
 * This class contains flow-insensitive information about fields and method
 * return values, i.e. it can tells us if a field or a return value is constant
 * throughout the entire program.
 *
 * It should never be written to as part of the inter/intra-procedural fixpoint
 * iteration process. Instead, it takes the results of a completed fixpoint
 * iteration and extracts the constant values.
 */
class WholeProgramState {
 public:
  // By default, the field and method partitions are initialized to Bottom.
  WholeProgramState() = default;

  // By default, the field and method partitions are initialized to Bottom.
  explicit WholeProgramState(
      const std::unordered_set<const DexType*>& field_blocklist)
      : m_field_blocklist(field_blocklist) {}

  // By default, the field and method partitions are initialized to Bottom.
  explicit WholeProgramState(
      std::shared_ptr<const call_graph::Graph> call_graph)
      : m_call_graph(std::move(call_graph)) {}

  WholeProgramState(const Scope&,
                    const interprocedural::FixpointIterator&,
                    const std::unordered_set<DexMethod*>&,
                    const std::unordered_set<const DexType*>&,
                    const std::unordered_set<const DexField*>&,
                    std::shared_ptr<const call_graph::Graph> call_graph);

  /*
   * If we only have knowledge of the constant values in a single class --
   * instead of a view of the constants in the whole program -- we can still
   * determine that the values of final fields are constant throughout
   * the entire program. This method records the values of those fields in the
   * WholeProgramState.
   */
  void collect_static_finals(const DexClass*, FieldEnvironment);

  void collect_instance_finals(const DexClass*,
                               const EligibleIfields&,
                               FieldEnvironment);

  void set_to_top() {
    m_field_partition.set_to_top();
    m_method_partition.set_to_top();
  }

  bool leq(const WholeProgramState& other) const {
    return m_field_partition.leq(other.m_field_partition) &&
           m_method_partition.leq(other.m_method_partition);
  }

  /*
   * Returns our best approximation of the field value.
   *
   * It will never return Bottom.
   */
  ConstantValue get_field_value(const DexField* field) const {
    if (!m_known_fields.count(field)) {
      return ConstantValue::top();
    }
    return m_field_partition.get(field);
  }

  /*
   * Returns our best static approximation of the return value.
   *
   * This may return Bottom to indicate that a method never returns (i.e. it
   * throws or loops indefinitely).
   */
  ConstantValue get_return_value(const DexMethod* method) const {
    if (!m_known_methods.count(method)) {
      return ConstantValue::top();
    }
    return m_method_partition.get(method);
  }

  const ConstantFieldPartition& get_field_partition() const {
    return m_field_partition;
  }

  const ConstantMethodPartition& get_method_partition() const {
    return m_method_partition;
  }

  bool has_call_graph() const { return !!m_call_graph; }

  const call_graph::Graph* call_graph() const { return m_call_graph.get(); }

  bool invoke_is_dynamic(const IRInstruction* insn) const {
    return call_graph::invoke_is_dynamic(*m_call_graph, insn);
  }

 private:
  void collect(
      const Scope& scope,
      const interprocedural::FixpointIterator& fp_iter,
      const std::unordered_set<const DexField*>& definitely_assigned_ifields);

  void collect_field_values(
      const IRInstruction* insn,
      const ConstantEnvironment& env,
      const DexType* clinit_cls,
      ConcurrentMap<const DexField*, ConstantValue>* fields_value_tmp);

  void collect_return_values(
      const IRInstruction* insn,
      const ConstantEnvironment& env,
      const DexMethod* method,
      ConcurrentMap<const DexMethod*, ConstantValue>* methods_value_tmp);

  std::shared_ptr<const call_graph::Graph> m_call_graph;

  // Unknown fields and methods will be treated as containing / returning Top.
  std::unordered_set<const DexField*> m_known_fields;
  std::unordered_set<const DexMethod*> m_known_methods;

  std::unordered_set<const DexType*> m_field_blocklist;

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
  ConstantFieldPartition m_field_partition;
  ConstantMethodPartition m_method_partition;
};

struct WholeProgramStateAccessorRecord {
  std::unordered_map<const DexField*, ConstantValue> field_dependencies;
  std::unordered_map<const DexMethod*, ConstantValue> method_dependencies;
};

class WholeProgramStateAccessor {
 public:
  explicit WholeProgramStateAccessor(const WholeProgramState& wps)
      : m_wps(wps) {}

  bool has_call_graph() const { return m_wps.has_call_graph(); }

  bool invoke_is_dynamic(const IRInstruction* insn) const {
    return m_wps.invoke_is_dynamic(insn);
  }

  ConstantValue get_field_value(const DexField* field) const {
    auto val = m_wps.get_field_value(field);
    if (m_record) {
      m_record->field_dependencies.emplace(field, val);
      return val;
    }
    return val;
  }

  ConstantValue get_return_value_from_cg(const IRInstruction* insn) const {
    const auto& callees =
        call_graph::resolve_callees_in_graph(*m_wps.call_graph(), insn);
    if (callees.empty()) {
      return ConstantValue::top();
    }
    for (const DexMethod* callee : callees) {
      if (!callee->get_code()) {
        return ConstantValue::top();
      }
    }
    ConstantValue ret = ConstantValue::bottom();
    for (const DexMethod* callee : callees) {
      const auto& val = m_wps.get_method_partition().get(callee);
      if (m_record) {
        m_record->method_dependencies.emplace(callee, val);
      }
      ret.join_with(val);
    }
    if (ret == ConstantValue::bottom()) {
      return ConstantValue::top();
    }
    return ret;
  }

  ConstantValue get_return_value(const DexMethod* method) const {
    auto val = m_wps.get_return_value(method);
    if (m_record) {
      m_record->method_dependencies.emplace(method, val);
    }
    return val;
  }

  void start_recording(WholeProgramStateAccessorRecord* record) {
    m_record = record;
  }

  void stop_recording() { m_record = nullptr; }

 private:
  const WholeProgramState& m_wps;
  WholeProgramStateAccessorRecord* m_record{nullptr};
};

/*
 * Incorporate information about the values of static fields and the return
 * values of other methods in the local analysis of a given method.
 */
class WholeProgramAwareAnalyzer final
    : public InstructionAnalyzerBase<WholeProgramAwareAnalyzer,
                                     ConstantEnvironment,
                                     const WholeProgramStateAccessor*> {
 public:
  static bool analyze_sget(const WholeProgramStateAccessor* whole_program_state,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_iget(const WholeProgramStateAccessor* whole_program_state,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_invoke(
      const WholeProgramStateAccessor* whole_program_state,
      const IRInstruction* insn,
      ConstantEnvironment* env);
};

} // namespace constant_propagation
