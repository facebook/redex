/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "PassManager.h"

namespace cse_impl {

struct Stats {
  size_t results_captured{0};
  size_t instructions_eliminated{0};
  size_t max_value_ids{0};
};

struct MethodBarriersStats {
  size_t inlined_barriers_iterations{0};
  size_t inlined_barriers_into_methods{0};
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

enum SpecialLocations : size_t {
  GENERAL_MEMORY_BARRIER,
  ARRAY_COMPONENT_TYPE_INT,
  ARRAY_COMPONENT_TYPE_BYTE,
  ARRAY_COMPONENT_TYPE_CHAR,
  ARRAY_COMPONENT_TYPE_WIDE,
  ARRAY_COMPONENT_TYPE_SHORT,
  ARRAY_COMPONENT_TYPE_OBJECT,
  ARRAY_COMPONENT_TYPE_BOOLEAN,
  END
};

// A (tracked) location is either a special location, or a field.
// Stored in a union, special locations are effectively represented as illegal
// pointer values.
// The nullptr field and SpecialLocations::OTHER_LOCATION are in effect aliases.
struct Location {
  explicit Location(const DexField* f) : field(f) {}
  explicit Location(SpecialLocations sl) : special_location(sl) {}
  union {
    const DexField* field;
    SpecialLocations special_location;
  };
};

inline bool operator==(const Location& a, const Location& b) {
  return a.field == b.field;
}

inline bool operator!=(const Location& a, const Location& b) {
  return !(a == b);
}

inline bool operator<(const Location& a, const Location& b) {
  if (a.special_location < SpecialLocations::END) {
    if (b.special_location < SpecialLocations::END) {
      return a.special_location < b.special_location;
    } else {
      return true;
    }
  }
  if (b.special_location < SpecialLocations::END) {
    return false;
  }
  return dexfields_comparator()(a.field, b.field);
}

struct LocationHasher {
  size_t operator()(const Location& l) const { return (size_t)l.field; }
};

class SharedState {
 public:
  SharedState();
  MethodBarriersStats init_method_barriers(const Scope&, size_t);
  boost::optional<Location> get_relevant_written_location(
      const IRInstruction* insn,
      const std::unordered_set<Location, LocationHasher>& read_locations);
  void log_barrier(const Barrier& barrier);
  void cleanup();

 private:
  bool may_be_barrier(const IRInstruction* insn);
  bool is_invoke_safe(const IRInstruction* insn);
  bool is_invoke_a_barrier(
      const IRInstruction* insn,
      const std::unordered_set<Location, LocationHasher>& read_locations);
  std::unordered_set<DexMethodRef*> m_safe_methods;
  std::unordered_set<DexType*> m_safe_types;
  std::unique_ptr<ConcurrentMap<Barrier, size_t, BarrierHasher>> m_barriers;
  std::unordered_map<const DexMethod*,
                     std::unordered_set<Location, LocationHasher>>
      m_method_written_locations;
  std::unique_ptr<const method_override_graph::Graph> m_method_override_graph;
};

class CommonSubexpressionElimination {
 public:
  CommonSubexpressionElimination(SharedState* shared_state,
                                 cfg::ControlFlowGraph&);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  bool patch(bool is_static, DexType* declaring_type, DexTypeList* args);

 private:
  // CSE is finding instances where the result (in the dest register) of an
  // earlier instruction can be forwarded to replace the result of another
  // (later) instruction.
  struct Forward {
    IRInstruction* earlier_insn;
    IRInstruction* insn;
  };
  std::vector<Forward> m_forward;
  SharedState* m_shared_state;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;
};

} // namespace cse_impl

class CommonSubexpressionEliminationPass : public Pass {
 public:
  CommonSubexpressionEliminationPass()
      : Pass("CommonSubexpressionEliminationPass") {}

  virtual void bind_config() override;
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  int64_t m_max_iterations;
  bool m_debug;
};
