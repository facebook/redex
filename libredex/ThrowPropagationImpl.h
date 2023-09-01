/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

namespace throw_propagation_impl {

class ThrowPropagator {
 public:
  ThrowPropagator(cfg::ControlFlowGraph& cfg, bool debug)
      : m_cfg(cfg), m_debug(debug) {}

  bool try_apply(const cfg::InstructionIterator& cfg_it);

 private:
  bool will_throw_or_not_terminate(cfg::InstructionIterator it);
  bool check_if_dead_code_present_and_prepare_block(
      const cfg::InstructionIterator& cfg_it);
  void insert_throw(const cfg::InstructionIterator& cfg_it);

  cfg::ControlFlowGraph& m_cfg;
  bool m_debug;
  boost::optional<std::pair<reg_t, reg_t>> m_regs;
};

} // namespace throw_propagation_impl
