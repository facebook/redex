/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Purity.h"

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
  size_t skipped_due_to_too_many_registers{0};
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
  SharedState(const std::unordered_set<DexMethodRef*>& pure_methods);
  void init_scope(const Scope&);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn,
      DexType* exact_virtual_scope,
      const CseUnorderedLocationSet& read_locations);
  void log_barrier(const Barrier& barrier);
  bool has_pure_method(const IRInstruction* insn) const;
  void cleanup();
  const CseUnorderedLocationSet&
  get_read_locations_of_conditionally_pure_method(
      const DexMethodRef* method_ref, IROpcode opcode) const;
  const SharedStateStats& get_stats() const { return m_stats; }
  const std::unordered_set<DexMethodRef*>& get_pure_methods() const {
    return m_pure_methods;
  }

 private:
  void init_method_barriers(const Scope& scope);
  bool may_be_barrier(const IRInstruction* insn, DexType* exact_virtual_scope);
  bool is_invoke_safe(const IRInstruction* insn, DexType* exact_virtual_scope);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn, const CseUnorderedLocationSet& read_locations);
  // after init_scope, m_pure_methods will include m_conditionally_pure_methods
  std::unordered_set<DexMethodRef*> m_pure_methods;
  std::unordered_set<DexMethodRef*> m_safe_methods;
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
                                 cfg::ControlFlowGraph&);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  bool patch(bool is_static,
             DexType* declaring_type,
             DexTypeList* args,
             unsigned int max_estimated_registers,
             bool runtime_assertions = false);

 private:
  // CSE is finding instances where the result (in the dest register) of an
  // earlier instruction can be forwarded to replace the result of another
  // (later) instruction.
  struct Forward {
    const IRInstruction* earlier_insn;
    IRInstruction* insn;
  };
  std::vector<Forward> m_forward;
  std::unordered_set<const IRInstruction*> m_earlier_insns;
  SharedState* m_shared_state;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;

  void insert_runtime_assertions(
      bool is_static,
      DexType* declaring_type,
      DexTypeList* args,
      const std::vector<std::pair<Forward, IRInstruction*>>& to_check);
};

} // namespace cse_impl
