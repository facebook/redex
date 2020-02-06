/*
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
  using InstructionSet = std::map<reg_t, IRInstruction*>;
  using ExtraLoads = std::unordered_map<cfg::Block*, InstructionSet>;

  static bool has_src(IRInstruction* insn, reg_t reg);

  SwitchEquivFinder(cfg::ControlFlowGraph* cfg,
                    const cfg::InstructionIterator& root_branch,
                    reg_t switching_reg,
                    uint32_t leaf_duplication_threshold = 0);

  SwitchEquivFinder() = delete;
  SwitchEquivFinder(const SwitchEquivFinder&) = delete;

  // After construction, call `success()`, to find out if a control flow
  // structure equivalent to a switch has been found
  bool success() const { return m_success; }
  const KeyToCase& key_to_case() const { return m_key_to_case; }
  const ExtraLoads& extra_loads() const { return m_extra_loads; }

  // Return all the blocks traversed by the finder, including leaves and
  // non-leaves.
  const std::vector<cfg::Block*> visited_blocks() const;

 private:
  std::vector<cfg::Edge*> find_leaves();
  void normalize_extra_loads(const std::unordered_set<cfg::Block*>& non_leaves);
  bool move_edges(
      const std::vector<std::pair<cfg::Edge*, cfg::Block*>>& edges_to_move);
  void find_case_keys(const std::vector<cfg::Edge*>& leaves);

  cfg::ControlFlowGraph* m_cfg;
  // The top-most branch instruction of the tree
  const cfg::InstructionIterator& m_root_branch;
  // The register that holds the value that we're "switching" on, even if this
  // is an if-else chain and not a switch statement
  reg_t m_switching_reg;
  // When D8 converts a switch statement into an if-else chain (and constant
  // loads are lifted), then a case block may be deduplicated. The deduplicated
  // case block can have multiple incoming edges with different program states
  // on each edge. This situation is impossible to represent with a switch
  // statement because there is no place to change the state of the program
  // between a switch statment and its case blocks (where there is for an
  // if-else chain).
  //
  // The SwitchEquivFinder could represent this situation as aswitch if case
  // blocks like this are duplicated. Each different program state is directed
  // to a different copy of the block. This way, each block has a separate set
  // of extra_loads. If a block has fewer than `m_leaf_duplication_threshold`
  // opcodes the SwitchEquivFinder may duplicate that block. If this flag is
  // zero, the SwitchEquivFinder will not edit the CFG.
  uint32_t m_leaf_duplication_threshold{0};
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

  // This stores the blocks visited and how many times in building m_key_to_case
  // Note that this does not include the root branch.
  std::unordered_map<cfg::Block*, uint16_t> m_visit_count;
};
