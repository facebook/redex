/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "ControlFlow.h"
#include "Dominators.h"
#include "MonotonicFixpointIterator.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"

class DexMethod;
class DexMethodRef;
class DexString;

// The SourceBlockConsistencyCheck class implements a simple consistency check
// which can be run after each phase to ensure that no phase removes source
// blocks in a way which is inconsistent with the source blocks' dominator
// tree.
//
// It is off by default.  It runs as part of the assessor, and may be emabled
// by adding '"run_sb_consistency": true' under "assessor" properties in the
// config JSON.
//
// At the end of InsertSourceBlocks, a SourceBlockDomInfo is created for the
// current method. This consists of a dominator tree for the original source
// blocks of the method (SourceBlockDomTree), along with APIs for querying
// which source blocks are legally removeable from the current method, and to
// actually remove the representation of a source block from the
// SourceBlockDomTree.
//
// After each pass, source blocks which are present in each method's IR are
// compared with the original set. Any which are missing, and which are not
// legally removeable according to the SourceBlockDomTree, will be reported as
// "missing". The set of removed source blocks is stored so that missing source
// blocks are only reported just after the pass they were removed in.
//
// Only leaves in the source blocks' dominator tree are legally removeable
// (after which, the source block can be removed from the dominator tree
// itself, potentially creating new leaves).  In practice, since this check
// runs after an entire pass, it validates that the set of removed source
// blocks could have been removed legally, but doesn't strictly validate that
// they were removed in the correct order.
//
// Recalculation of the source blocks' dominator tree is currently never done.
// This could lead to false negatives, for example:
//
//   CFG:           Dom Tree:
//        A                   A
//       / \                 /|\
//      C   B               B C D
//       \ /
//        D
//
// It's not legal to remove B and C without removing D. Recalc'ing after
// removing B or C would make C or B D's immediate dominator, but without that,
// removing B and C is reported as legal. However, this should only over-report
// leaves, and thus shouldn't cause false positives to be reported.
//
// Inlining also isn't accounted for as a result. In future, the dom tree
// should be recalculated at strategic points.
//
// Note: Under the hood, SourceBlockDomTree is really represented by a "flipped"
// dominator tree, i.e. a DAG where edges symbolize "is dominated by"
// relations.

namespace source_blocks {

struct SourceBlockInfo {
  const DexString* original_dex_method;
  uint32_t id;
};

constexpr SourceBlockInfo kInvalidSBI =
    SourceBlockInfo{nullptr, std::numeric_limits<uint32_t>::max()};

bool operator<(const SourceBlockInfo& l, const SourceBlockInfo& r);
bool operator==(const SourceBlockInfo& l, const SourceBlockInfo& r);

enum class SourceBlockDomTreeKind { Dom, PostDom };

struct DomTreeNode {
  SourceBlockInfo imm_dom = kInvalidSBI;
  uint32_t in_degree = 0;
};

template <SourceBlockDomTreeKind Kind>
class SourceBlockDomTree {
  using GraphInterfaceT = std::conditional_t<
      Kind == SourceBlockDomTreeKind::Dom,
      cfg::GraphInterface,
      sparta::BackwardsFixpointIterationAdaptor<cfg::GraphInterface>>;

 public:
  SourceBlockDomTree() = default;
  SourceBlockDomTree(const cfg::ControlFlowGraph& cfg, uint32_t num_src_blks);

  const std::set<SourceBlockInfo>& leaves();

  void remove_src_blk(const SourceBlockInfo& sb_info);

 private:
  std::map<SourceBlockInfo, DomTreeNode> m_dom_tree_nodes;
  std::set<SourceBlockInfo> m_leaves;
};

class SourceBlockDomInfo {
 public:
  SourceBlockDomInfo() = default;
  SourceBlockDomInfo(const cfg::ControlFlowGraph& cfg, uint32_t num_src_blks);

  std::vector<SourceBlockInfo> get_removeable_src_blks();

  void remove_src_blk(const SourceBlockInfo& sb_info);

 private:
  SourceBlockDomTree<SourceBlockDomTreeKind::Dom> m_dom_tree;
};

struct SBConsistencyContext {
  std::set<SourceBlockInfo> m_source_blocks;
  std::set<SourceBlockInfo> m_known_missing_source_blocks;
  source_blocks::SourceBlockDomInfo m_sbdi;
};

class SourceBlockConsistencyCheck {
 public:
  SourceBlockConsistencyCheck() = default;

  void initialize(const Scope& scope);

  bool is_initialized() const;
  size_t run(const Scope& scope);

 private:
  void rebuild_sbdi(DexMethod* dex_method, const cfg::ControlFlowGraph& cfg);

  std::unordered_map<DexMethod*, SBConsistencyContext> m_context_map;

  bool m_is_initialized = false;
};

// inline functions

inline bool SourceBlockConsistencyCheck::is_initialized() const {
  return m_is_initialized;
}

// Template implementations:

template <SourceBlockDomTreeKind Kind>
SourceBlockDomTree<Kind>::SourceBlockDomTree(const cfg::ControlFlowGraph& cfg,
                                             uint32_t num_src_blks) {
  if (cfg.blocks().empty()) {
    return;
  }

  auto first_block = *cfg.blocks().begin();
  always_assert(first_block);
  auto first_sb = source_blocks::get_first_source_block(first_block);
  always_assert(first_sb);
  auto* dex_method = first_sb->src;

  for (uint32_t i = 0; i < num_src_blks; i++) {
    m_leaves.insert({dex_method, i});
  }

  auto doms = dominators::SimpleFastDominators<GraphInterfaceT>(cfg);

  for (cfg::Block* block : cfg.blocks()) {
    if (block == cfg.exit_block() &&
        cfg.get_pred_edge_of_type(block, EDGE_GHOST)) {
      continue;
    }

    source_blocks::foreach_source_block(
        block, [num_src_blks, dex_method, this](auto& sb) {
          always_assert(sb);
          always_assert(sb->id < num_src_blks);
          always_assert(sb->src == dex_method);

          auto sb_info = SourceBlockInfo{sb->src, sb->id};
          m_dom_tree_nodes[sb_info] = {};
        });
  }

  for (cfg::Block* block : cfg.blocks()) {
    if (block == cfg.exit_block() &&
        cfg.get_pred_edge_of_type(block, EDGE_GHOST)) {
      continue;
    }

    SourceBlock* prev = nullptr;
    auto first_sb_in_block = source_blocks::get_first_source_block(block);

    if (!first_sb_in_block) {
      continue;
    }

    source_blocks::foreach_source_block(
        block, [this, first_sb_in_block, &prev](const auto& sb) {
          if (sb != first_sb_in_block) {
            always_assert(prev);
            auto prev_sb_info = SourceBlockInfo{prev->src, prev->id};

            auto curr_sb_info = SourceBlockInfo{sb->src, sb->id};
            if constexpr (Kind == SourceBlockDomTreeKind::Dom) {
              m_dom_tree_nodes.at(curr_sb_info).imm_dom = prev_sb_info;
              m_dom_tree_nodes.at(prev_sb_info).in_degree++;
              m_leaves.erase(prev_sb_info);
            } else {
              m_dom_tree_nodes.at(prev_sb_info).imm_dom = curr_sb_info;
              m_dom_tree_nodes.at(curr_sb_info).in_degree++;
              m_leaves.erase(curr_sb_info);
            }
          }

          prev = sb;
        });

    auto curr_idom = doms.get_idom(block);
    if (!curr_idom || (curr_idom == cfg.exit_block() &&
                       cfg.get_pred_edge_of_type(curr_idom, EDGE_GHOST))) {
      continue;
    }

    // In the idom implementation in Dominators.h, the entry block's idom is
    // set to itself, which is not correct according to the definition of an
    // idom (requires strict dominance - should be null for the entry block),
    // but let's not risk breaking anything else by fixing that.
    if (curr_idom == block) {
      continue;
    }

    SourceBlock* sb_in_idom = nullptr;

    if constexpr (Kind == SourceBlockDomTreeKind::Dom) {
      sb_in_idom = source_blocks::get_last_source_block(curr_idom);
    } else {
      sb_in_idom = source_blocks::get_first_source_block(curr_idom);
    }

    always_assert(sb_in_idom);
    always_assert(sb_in_idom->id < num_src_blks);
    always_assert(sb_in_idom->src == dex_method);

    auto sb_in_idom_info = SourceBlockInfo{sb_in_idom->src, sb_in_idom->id};

    m_leaves.erase(sb_in_idom_info);
    m_dom_tree_nodes.at(sb_in_idom_info).in_degree++;

    if constexpr (Kind == SourceBlockDomTreeKind::Dom) {
      auto first_sb_in_block_info =
          SourceBlockInfo{first_sb_in_block->src, first_sb_in_block->id};
      m_dom_tree_nodes.at(first_sb_in_block_info).imm_dom = sb_in_idom_info;
    } else {
      auto last_sb_in_block = source_blocks::get_last_source_block(curr_idom);
      auto last_sb_in_block_info =
          SourceBlockInfo{last_sb_in_block->src, last_sb_in_block->id};
      m_dom_tree_nodes.at(last_sb_in_block_info).imm_dom = sb_in_idom_info;
    }
  }
}

template <SourceBlockDomTreeKind Kind>
void SourceBlockDomTree<Kind>::remove_src_blk(const SourceBlockInfo& sb_info) {
  {
    auto it = m_leaves.find(sb_info);
    always_assert(it != m_leaves.end());

    m_leaves.erase(it);
  }

  auto it1 = m_dom_tree_nodes.find(sb_info);
  always_assert(it1 != m_dom_tree_nodes.end());

  auto& node = it1->second;
  always_assert(node.in_degree == 0);

  if (!(node.imm_dom == kInvalidSBI)) {
    auto it2 = m_dom_tree_nodes.find(node.imm_dom);
    always_assert(it2 != m_dom_tree_nodes.end());

    auto& imm_dom_node = it2->second;
    always_assert(imm_dom_node.in_degree > 0);

    imm_dom_node.in_degree--;
    if (imm_dom_node.in_degree == 0) {
      m_leaves.insert(node.imm_dom);
    }
  }

  node = DomTreeNode{kInvalidSBI, std::numeric_limits<uint32_t>::max()};
}

template <SourceBlockDomTreeKind Kind>
const std::set<SourceBlockInfo>& SourceBlockDomTree<Kind>::leaves() {
  return m_leaves;
}
} // namespace source_blocks
