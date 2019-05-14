/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class CommonSubexpressionElimination {
 public:
  struct Stats {
    size_t results_captured{0};
    size_t instructions_eliminated{0};
  };

  class SharedState {
   public:
    SharedState();
    bool is_invoke_a_barrier(const IRInstruction* insn);

   private:
    std::unordered_set<DexMethodRef*> m_safe_methods;
    std::unordered_set<DexType*> m_safe_types;
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

class CommonSubexpressionEliminationPass : public Pass {
 public:
  CommonSubexpressionEliminationPass()
      : Pass("CommonSubexpressionEliminationPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
