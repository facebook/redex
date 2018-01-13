/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <mutex>

#include "ConstPropConfig.h"
#include "ConstantEnvironment.h"
#include "IRCode.h"
#include "LocalConstProp.h"
#include "Pass.h"

/**
 * Intraprocedural Constant propagation
 *
 * This code leverages the analysis built by LocalConstantPropagation
 * which works at the basic block level and extends its capabilities by
 * leveraging the Abstract Interpretation Framework's FixpointIterator
 * and HashedAbstractEnvironment facilities.
 *
 * By running the fixpoint iterator, instead of having no knowledge at
 * the start of a basic block, we can now run the analysis with constants
 * that have been propagated beyond the basic block boundary.
 *
 * This code works in two phases:
 *
 * Phase 1:  First, gather all the facts about constant and model them inside
 *           the constants lattice (described above). Run the fixpoint analysis
 *           and propagate all facts throughout the CFG. In code these are all
 *           the analyze_*() functions.
 *
 * Phase 2:  Once we reached a fixpoint, then replay the analysis but this
 *           time use the previously gathered facts about constant and use them
 *           to replace instructions.  These are all the simplify_* functions.
 */
class IntraProcConstantPropagation final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       ConstantEnvironment> {
 public:
  explicit IntraProcConstantPropagation(ControlFlowGraph& cfg,
                                        const ConstPropConfig& config)
      : MonotonicFixpointIterator(cfg),
        m_cfg(cfg),
        m_config(config),
        m_lcp{config} {}

  ConstantEnvironment analyze_edge(
      const std::shared_ptr<cfg::Edge>&,
      const ConstantEnvironment& exit_state_at_source) const override;

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state) const;

  void analyze_node(const NodeId& block,
                    ConstantEnvironment* state_at_entry) const override;

  void simplify_instruction(Block* const& block,
                            IRInstruction* insn,
                            const ConstantEnvironment& current_state) const;

  void simplify() const;

  void apply_changes(IRCode*) const;

  size_t branches_removed() const { return m_lcp.num_branch_propagated(); }

  size_t materialized_consts() const { return m_lcp.num_materialized_consts(); }

 private:
  const ControlFlowGraph& m_cfg;
  const ConstPropConfig m_config;
  mutable LocalConstantPropagation m_lcp;
};

class ConstantPropagationPass : public Pass {
 public:
  ConstantPropagationPass() : Pass("ConstantPropagationPass") {}

  virtual void configure_pass(const PassConfig& pc) override;
  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) override;


 private:
  ConstPropConfig m_config;

  // stats
  std::mutex m_stats_mutex;
  size_t m_branches_removed{0};
  size_t m_materialized_consts{0};
};
