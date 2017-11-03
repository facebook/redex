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
#include "GlobalConstProp.h"
#include "IRCode.h"
#include "LocalConstProp.h"
#include "Pass.h"

using std::placeholders::_1;
using std::vector;

/** Intraprocedural Constant propagation
 * This code leverages the analysis built by LocalConstantPropagation
 * with works at the basic block level and extends its capabilities by
 * leveraging the Abstract Interpretation Framework's FixPointIterator
 * and HashedAbstractEnvironment facilities.
 *
 * By running the fix point iterator, instead of having no knowledge at
 * the start of a basic block, we can now run the analysis with constants
 * that have been propagated beyond the basic block boundary making this
 * more powerful than its predecessor pass.
 */
class IntraProcConstantPropagation final
    : public ConstantPropFixpointAnalysis<cfg::GraphInterface,
                                          MethodItemEntry,
                                          std::vector<Block*>,
                                          InstructionIterable> {
 public:
  explicit IntraProcConstantPropagation(ControlFlowGraph& cfg,
                                        const ConstPropConfig& config)
      : ConstantPropFixpointAnalysis<cfg::GraphInterface,
                                     MethodItemEntry,
                                     vector<Block*>,
                                     InstructionIterable>(cfg, cfg.blocks()),
        m_config(config),
        m_lcp{config} {}

  ConstPropEnvironment analyze_edge(
      const std::shared_ptr<cfg::Edge>&,
      const ConstPropEnvironment& exit_state_at_source) const override;

  void simplify_instruction(
      Block* const& block,
      MethodItemEntry& mie,
      const ConstPropEnvironment& current_state) const override;
  void analyze_instruction(const MethodItemEntry& mie,
                           ConstPropEnvironment* current_state) const override;
  void apply_changes(IRCode*) const;

  size_t branches_removed() const { return m_lcp.num_branch_propagated(); }
  size_t materialized_consts() const { return m_lcp.num_materialized_consts(); }

 private:
  const ConstPropConfig m_config;
  mutable LocalConstantPropagation m_lcp;
};

class ConstantPropagationPass : public Pass {
 public:
  ConstantPropagationPass()
      : Pass("ConstantPropagationPass"), m_branches_removed(0) {}

  virtual void configure_pass(const PassConfig& pc) override;
  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) override;


 private:
  ConstPropConfig m_config;

  // stats
  std::mutex m_stats_mutex;
  size_t m_branches_removed;
  size_t m_materialized_consts;
};
