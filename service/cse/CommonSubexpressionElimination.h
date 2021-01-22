/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "IROpcode.h"
#include "MethodOverrideGraph.h"
#include "PatriciaTreeSet.h"
#include "Purity.h"

class DexField;
class DexMethod;
class DexMethodRef;
class DexType;
class IRInstruction;

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace cse_impl {

struct Stats {
  size_t results_captured{0};
  size_t stores_captured{0};
  size_t array_lengths_captured{0};
  size_t instructions_eliminated{0};
  size_t max_value_ids{0};
  size_t methods_using_other_tracked_location_bit{0};
  // keys are IROpcode encoded as uint16_t, to make OSS build happy
  std::unordered_map<uint16_t, size_t> eliminated_opcodes;
  size_t max_iterations{0};

  Stats& operator+=(const Stats&);
};

struct SharedStateStats {
  size_t method_barriers{0};
  size_t method_barriers_iterations{0};
  size_t conditionally_pure_methods{0};
  size_t conditionally_pure_methods_iterations{0};
};

// A barrier is defined by a particular opcode, and possibly some extra data
// (field, method)
struct Barrier {
  IROpcode opcode;
  union {
    const DexField* field{nullptr};
    const DexMethod* method;
  };
};

inline bool operator==(const Barrier& a, const Barrier& b) {
  return a.opcode == b.opcode && a.field == b.field;
}

struct BarrierHasher {
  size_t operator()(const Barrier& b) const {
    return b.opcode ^ (size_t)b.field;
  }
};

class SharedState {
 public:
  explicit SharedState(
      const std::unordered_set<DexMethodRef*>& pure_methods,
      const std::unordered_set<DexString*>& finalish_field_names);
  void init_scope(const Scope&);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn,
      DexType* exact_virtual_scope,
      const CseUnorderedLocationSet& read_locations);
  void log_barrier(const Barrier& barrier);
  bool has_pure_method(const IRInstruction* insn) const;
  bool is_finalish(const DexField* field) const;
  void cleanup();
  const CseUnorderedLocationSet&
  get_read_locations_of_conditionally_pure_method(
      const DexMethodRef* method_ref, IROpcode opcode) const;
  const SharedStateStats& get_stats() const { return m_stats; }
  const std::unordered_set<DexMethodRef*>& get_pure_methods() const {
    return m_pure_methods;
  }
  const method_override_graph::Graph* get_method_override_graph() const;

 private:
  void init_method_barriers(const Scope& scope);
  bool may_be_barrier(const IRInstruction* insn, DexType* exact_virtual_scope);
  bool is_invoke_safe(const IRInstruction* insn, DexType* exact_virtual_scope);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn, const CseUnorderedLocationSet& read_locations);
  // after init_scope, m_pure_methods will include m_conditionally_pure_methods
  std::unordered_set<DexMethodRef*> m_pure_methods;
  // methods which never represent barriers
  std::unordered_set<DexMethodRef*> m_safe_methods;
  // subset of safe methods which are in fact defs
  std::unordered_set<const DexMethod*> m_safe_method_defs;
  const std::unordered_set<DexString*>& m_finalish_field_names;
  std::unique_ptr<ConcurrentMap<Barrier, size_t, BarrierHasher>> m_barriers;
  std::unordered_map<const DexMethod*, CseUnorderedLocationSet>
      m_method_written_locations;
  std::unordered_map<const DexMethod*, CseUnorderedLocationSet>
      m_conditionally_pure_methods;
  std::unique_ptr<const method_override_graph::Graph> m_method_override_graph;
  SharedStateStats m_stats;
};

class CommonSubexpressionElimination {
 public:
  CommonSubexpressionElimination(SharedState* shared_state,
                                 cfg::ControlFlowGraph&,
                                 bool is_static,
                                 bool is_init_or_clinit,
                                 DexType* declaring_type,
                                 DexTypeList* args);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  bool patch(bool runtime_assertions = false);

 private:
  // CSE is finding instances where the result (in the dest register) of an
  // earlier instruction can be forwarded to replace the result of another
  // (later) instruction.
  struct Forward {
    // Index into m_earlier_insns
    size_t earlier_insns_index;
    IRInstruction* insn;
  };
  std::vector<Forward> m_forward;
  // List of unique sets of earlier instructions to be forwarded
  std::vector<sparta::PatriciaTreeSet<const IRInstruction*>> m_earlier_insns;
  SharedState* m_shared_state;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;
  bool m_is_static;
  DexType* m_declaring_type;
  DexTypeList* m_args;

  void insert_runtime_assertions(
      const std::vector<std::pair<Forward, IRInstruction*>>& to_check);
};

} // namespace cse_impl
