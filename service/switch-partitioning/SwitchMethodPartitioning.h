/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <unordered_map>
#include <vector>

#include "ConstantPropagationAnalysis.h"
#include "DexClass.h"
#include "IRCode.h"
#include "ScopedCFG.h"

/*
 * This is designed to work on methods with a very specific control-flow graph
 * -- methods whose sources contain a single switch statement (or if-else tree)
 * and no other control-flow structures (like catch blocks). We expect the CFG
 * to be of the following form:
 *
 *          [Prologue block(s)]  ____
 *         _/       |         \_     \______
 *        /         |  ...      \           \
 *    [case 0]   [case 1]  ... [case N]   [default case (may throw)]
 *        \_         |  ...    _/   _______/
 *          \        |        /  __/
 *           [Exit block(s)]
 *
 * We partition the method into these prologue blocks and case blocks. The
 * default case and the exit blocks, if any, are omitted. The current usages
 * of SMP have no need for the default case, and they can find the exit blocks
 * easily enough by following the successor edges from the case blocks.
 *
 * It's also possible that there are no exit blocks, rather each case has a
 * return opcode.
 *
 * SwitchMethodPartitioning is slightly a misnomer. It was originally designed
 * for methods that had a single switch statement, but was later extended to
 * support methods that use an if-else tree to choose a case block (instead of
 * a switch). These methods may have been switch-only in source code, but have
 * been compiled into if-else trees (usually by d8).
 */
class SwitchMethodPartitioning final {
 public:
  static std::unique_ptr<SwitchMethodPartitioning> create(
      IRCode* code, bool verify_default_case_throws = true);

  const std::vector<cfg::Block*>& get_prologue_blocks() const {
    return m_prologue_blocks;
  }

  const std::unordered_map<int32_t, cfg::Block*>& get_key_to_block() const {
    return m_key_to_block;
  }

 private:
  SwitchMethodPartitioning(
      cfg::ScopedCFG cfg,
      std::vector<cfg::Block*> prologue_blocks,
      std::unordered_map<int32_t, cfg::Block*> key_to_block)
      : m_prologue_blocks(std::move(prologue_blocks)),
        m_key_to_block(std::move(key_to_block)),
        m_cfg(std::move(cfg)) {}

  std::vector<cfg::Block*> m_prologue_blocks;
  std::unordered_map<int32_t, cfg::Block*> m_key_to_block;
  cfg::ScopedCFG m_cfg;
};
