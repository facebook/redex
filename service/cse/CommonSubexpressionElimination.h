/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/PatriciaTreeSet.h>

#include "ConcurrentContainers.h"
#include "DeterministicContainers.h"
#include "IROpcode.h"
#include "MethodOverrideGraph.h"
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
  UnorderedMap<uint16_t, size_t> eliminated_opcodes;
  size_t max_iterations{0};
  size_t branches_eliminated{0};

  Stats& operator+=(const Stats&);
};

struct SharedStateStats {
  size_t method_barriers{0};
  size_t method_barriers_iterations{0};
  size_t finalizable_fields{0};
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
      const UnorderedSet<DexMethodRef*>& pure_methods,
      const UnorderedSet<const DexString*>& finalish_field_names,
      const UnorderedSet<const DexField*>& finalish_fields);
  void init_scope(const Scope&,
                  const method::ClInitHasNoSideEffectsPredicate&
                      clinit_has_no_side_effects);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn,
      DexType* exact_virtual_scope,
      const CseUnorderedLocationSet& read_locations);
  void log_barrier(const Barrier& barrier);
  bool has_potential_unboxing_method(const IRInstruction* insn) const;
  bool has_pure_method(const IRInstruction* insn) const;
  bool is_finalish(const DexField* field) const;
  void cleanup();
  const CseUnorderedLocationSet&
  get_read_locations_of_conditionally_pure_method(
      const DexMethodRef* method_ref, IROpcode opcode) const;
  const SharedStateStats& get_stats() const { return m_stats; }
  const UnorderedSet<DexMethodRef*>& get_pure_methods() const {
    return m_pure_methods;
  }
  const method_override_graph::Graph* get_method_override_graph() const;
  const UnorderedSet<const DexField*>& get_finalizable_fields() const {
    return m_finalizable_fields;
  }

  const UnorderedMap<const DexMethodRef*, const DexMethodRef*>& get_boxing_map()
      const {
    return m_boxing_map;
  }

  const UnorderedMap<const DexMethodRef*, const DexMethodRef*>&
  get_abstract_map() const {
    return m_abstract_map;
  }

 private:
  void init_method_barriers(const Scope& scope);
  void init_finalizable_fields(const Scope& scope);
  bool may_be_barrier(const IRInstruction* insn, DexType* exact_virtual_scope);
  bool is_invoke_safe(const IRInstruction* insn, DexType* exact_virtual_scope);
  CseUnorderedLocationSet get_relevant_written_locations(
      const IRInstruction* insn, const CseUnorderedLocationSet& read_locations);
  // after init_scope, m_pure_methods will include m_conditionally_pure_methods
  UnorderedSet<DexMethodRef*> m_pure_methods;
  // methods which never represent barriers
  UnorderedSet<DexMethodRef*> m_safe_methods;
  // subset of safe methods which are in fact defs
  UnorderedSet<const DexMethod*> m_safe_method_defs;
  const UnorderedSet<const DexString*>& m_finalish_field_names;
  const UnorderedSet<const DexField*>& m_finalish_fields;
  UnorderedSet<const DexField*> m_finalizable_fields;
  std::unique_ptr<AtomicMap<Barrier, size_t, BarrierHasher>> m_barriers;
  UnorderedMap<const DexMethod*, CseUnorderedLocationSet>
      m_method_written_locations;
  UnorderedMap<const DexMethod*, CseUnorderedLocationSet>
      m_conditionally_pure_methods;
  std::unique_ptr<const method_override_graph::Graph> m_method_override_graph;
  SharedStateStats m_stats;
  // boxing to unboxing mapping
  UnorderedMap<const DexMethodRef*, const DexMethodRef*> m_boxing_map;
  // unboxing to its abstract mapping
  UnorderedMap<const DexMethodRef*, const DexMethodRef*> m_abstract_map;
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
  UnorderedMap<const IRInstruction*, size_t> m_earlier_insn_ids;
  // CSE is finding instances where the result (in the dest register) of an
  // earlier instruction can be forwarded to replace the result of another
  // (later) instruction.
  struct Forward {
    // Index into m_earlier_insns
    size_t earlier_insns_index;
    IRInstruction* insn;
  };
  std::vector<Forward> m_forward;
  std::vector<cfg::Edge*> m_dead_edges;
  // List of unique sets of earlier instructions to be forwarded
  std::vector<sparta::PatriciaTreeSet<const IRInstruction*>> m_earlier_insns;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;
  bool m_is_static;
  DexType* m_declaring_type;
  DexTypeList* m_args;

  std::vector<IRInstruction*> m_unboxing;
  const UnorderedMap<const DexMethodRef*, const DexMethodRef*>& m_abs_map;

  void insert_runtime_assertions(
      const std::vector<std::pair<Forward, IRInstruction*>>& to_check);

  size_t get_earlier_insn_id(const IRInstruction*);
};

} // namespace cse_impl
