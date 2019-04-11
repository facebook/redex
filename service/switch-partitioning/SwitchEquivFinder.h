/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <unordered_set>

#include "ControlFlow.h"

/**
 * The purpose of this class is to find control flow structures that are
 * equivalent to a switch statement. They can be any combination of ifs and
 * switches as long as the only intervening instructions are const loads that do
 * not overwrite the switching register.
 */
class SwitchEquivFinder final {
 public:
  using KeyToCase = std::map<boost::optional<int32_t>, cfg::Block*>;
  using InstructionSet = std::map<uint16_t, IRInstruction*>;
  using ExtraLoads = std::unordered_map<cfg::Block*, InstructionSet>;

  static bool has_src(IRInstruction* insn, uint16_t reg);

  SwitchEquivFinder(cfg::ControlFlowGraph* cfg,
                    const cfg::InstructionIterator& root_branch,
                    uint16_t switching_reg);

  SwitchEquivFinder() = delete;
  SwitchEquivFinder(const SwitchEquivFinder&) = delete;

  // After construction, call `success()`, to find out if a control flow
  // structure equivalent to a switch has been found
  bool success() const { return m_success; }
  const KeyToCase& key_to_case() const { return m_key_to_case; }
  const ExtraLoads& extra_loads() const { return m_extra_loads; }

 private:
  std::vector<cfg::Edge*> find_leaves();
  void normalize_extra_loads(std::unordered_set<cfg::Block*> non_leaves);
  void find_case_keys(const std::vector<cfg::Edge*>& leaves);

  cfg::ControlFlowGraph* m_cfg;
  // The top-most branch instruction of the tree
  const cfg::InstructionIterator& m_root_branch;
  // The register that holds the value that we're "switching" on, even if this
  // is an if-else chain and not a switch statement
  uint16_t m_switching_reg;
  // If a switch equivalent cannot be found starting from `m_root_branch` this
  // flag will be false, otherwise true.
  bool m_success{false};

  // A map from case keys to leaf blocks. The case key is the value held in
  // `m_switching_reg` upon reaching this leaf. `boost::none` represents the
  // fallthrough block.
  KeyToCase m_key_to_case;

  // This map represents the state of the registers upon entering a leaf block.
  // Any constant loads that occurred on all paths to a given leaf after the
  // root branch block are added to its InstructionSet. We use an ordered map
  // keyed by the destination register so that values can be overwritten and
  // iterated in a deterministic order
  ExtraLoads m_extra_loads;
};
