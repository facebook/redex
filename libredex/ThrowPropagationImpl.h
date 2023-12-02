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
  explicit ThrowPropagator(cfg::ControlFlowGraph& cfg) : m_cfg(cfg) {}

  bool try_apply(const cfg::InstructionIterator& cfg_it);

 private:
  bool will_throw_or_not_terminate_or_unreachable(cfg::InstructionIterator it);
  bool check_if_dead_code_present_and_prepare_block(
      const cfg::InstructionIterator& cfg_it);
  void insert_unreachable(const cfg::InstructionIterator& cfg_it);

  cfg::ControlFlowGraph& m_cfg;
  boost::optional<reg_t> m_reg;
};

} // namespace throw_propagation_impl
