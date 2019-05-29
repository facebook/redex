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

class CommonSubexpressionElimination {
 public:
  struct Stats {
    size_t results_captured{0};
    size_t instructions_eliminated{0};
  };

  struct Barrier {
    IROpcode opcode;
    union {
      DexField* field{nullptr};
      DexMethod* method;
    };
  };

  struct BarrierHasher {
    size_t operator()(const Barrier& b) const {
      return b.opcode ^ (size_t)b.field;
    }
  };

  enum AccessedArrayComponentTypes {
    INT,
    BYTE,
    CHAR,
    WIDE,
    SHORT,
    OBJECT,
    BOOLEAN,
    END
  };

  struct ReadAccesses {
    // A nullptr entry indicates that some read field could not be resolved;
    // known-to-be volatile fields never appear in this set.
    std::unordered_set<DexField*> fields;
    std::bitset<AccessedArrayComponentTypes::END> array_component_types;
  };

  struct MethodBarriersStats {
    size_t inlined_barriers_iterations{0};
    size_t inlined_barriers_into_methods{0};
  };

  class SharedState {
   public:
    SharedState();
    MethodBarriersStats init_method_barriers(const Scope&,
                                             size_t max_iterations);
    ReadAccesses compute_read_accesses(cfg::ControlFlowGraph&);
    bool is_barrier(const IRInstruction* insn,
                    const ReadAccesses& read_accesses);
    void log_barrier(const Barrier& barrier);
    void cleanup();

   private:
    bool may_be_barrier(const IRInstruction* insn);
    bool is_invoke_safe(const IRInstruction* insn);
    bool is_invoke_a_barrier(const IRInstruction* insn,
                             const ReadAccesses& read_accesses);
    bool is_barrier_relevant(const Barrier& barrier,
                             const ReadAccesses& read_accesses);
    std::unordered_set<DexMethodRef*> m_safe_methods;
    std::unordered_set<DexType*> m_safe_types;
    std::unique_ptr<ConcurrentMap<Barrier, size_t, BarrierHasher>> m_barriers;
    std::unordered_map<const DexMethod*, std::vector<Barrier>>
        m_method_barriers;
    std::unique_ptr<const method_override_graph::Graph> m_method_override_graph;
  };

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

inline bool operator==(const CommonSubexpressionElimination::Barrier& a,
                       const CommonSubexpressionElimination::Barrier& b) {
  return a.opcode == b.opcode && a.field == b.field;
}

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
