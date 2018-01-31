/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstPropConfig.h"
#include "ConstantEnvironment.h"
#include "IRCode.h"

namespace constant_propagation {

namespace intraprocedural {

class FixpointIterator final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       ConstantEnvironment> {
 public:
  /**
   * The fixpoint iterator takes an optional field_env argument that it will use
   * to determine the constant values (if any) of static fields encountered in
   * sget-* instructions.
   */
  explicit FixpointIterator(ControlFlowGraph& cfg,
                            const ConstPropConfig& config,
                            ConstantStaticFieldEnvironment field_env =
                                ConstantStaticFieldEnvironment())
      : MonotonicFixpointIterator(cfg),
        m_config(config),
        m_field_env(field_env) {}

  ConstantEnvironment analyze_edge(
      const std::shared_ptr<cfg::Edge>&,
      const ConstantEnvironment& exit_state_at_source) const override;

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state) const;

  void analyze_node(const NodeId& block,
                    ConstantEnvironment* state_at_entry) const override;

 private:
  const ConstPropConfig m_config;
  ConstantStaticFieldEnvironment m_field_env;
};

} // namespace intraprocedural

} // namespace constant_propagation
