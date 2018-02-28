/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "ConstantPropagationWholeProgramState.h"
#include "IRCode.h"

namespace constant_propagation {

namespace intraprocedural {

class FixpointIterator final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       ConstantEnvironment> {
 public:
  struct Config {
    bool fold_arithmetic{false};
    // If we are analyzing a class initializer, this is expected to point to
    // the DexType of the class. It indicates that the analysis can treat the
    // static fields of this class as non-escaping.
    DexType* class_under_init{nullptr};
  };

  /*
   * The fixpoint iterator takes an optional WholeProgramState argument that
   * it will use to determine the static field values and method return values.
   */
  explicit FixpointIterator(const ControlFlowGraph& cfg,
                            const Config config,
                            const WholeProgramState* wps = nullptr)
      : MonotonicFixpointIterator(cfg), m_config(config), m_wps(wps) {}

  explicit FixpointIterator(const ControlFlowGraph& cfg)
      : FixpointIterator(cfg, Config()) {}

  ConstantEnvironment analyze_edge(
      const std::shared_ptr<cfg::Edge>&,
      const ConstantEnvironment& exit_state_at_source) const override;

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state) const;

  void analyze_node(const NodeId& block,
                    ConstantEnvironment* state_at_entry) const override;

 private:
  const Config m_config;
  const WholeProgramState* m_wps;
};

} // namespace intraprocedural

} // namespace constant_propagation
