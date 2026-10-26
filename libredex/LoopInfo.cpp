/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoopInfo.h"

#include <algorithm>

#include "Debug.h"
#include "DeterministicContainers.h"

using namespace loop_impl;

/**
 * The head of the loop's component. For a reducible loop it is the only block
 * with predecessors outside of the loop, and it dominates every block in the
 * loop; otherwise it is just one of the loop's entries.
 */
cfg::Block* Loop::get_header() { return m_blocks.front(); }

/**
 * The only predecessor of the header, which therefore dominates every block of
 * the loop. Only make_with_preheaders creates one, and only for a reducible
 * loop; otherwise nullptr.
 */
cfg::Block* Loop::get_preheader() { return m_loop_preheader; }

/**
 * Get the loop immediately outside of the current loop, or nullptr for an
 * outermost loop.
 */
Loop* Loop::get_parent_loop() { return m_parent_loop; }

void Loop::set_preheader(cfg::Block* ph) { m_loop_preheader = ph; }

bool Loop::contains(Loop* l) const {
  if (l == this) {
    return true;
  }
  if (l == nullptr) {
    return false;
  }
  return contains(l->get_parent_loop());
}

bool Loop::contains(cfg::Block* block) const {
  return m_block_set.count(block) != 0u;
}

/**
 * The loop depth is the number of enclosing loops a loop has (a loop
 * encloses itself). This number will always be >= 1
 */
int Loop::get_loop_depth() const {
  int depth = 1;
  for (auto* current = m_parent_loop; current != nullptr;
       current = current->m_parent_loop) {
    ++depth;
  }
  return depth;
}

/**
 * Returns the blocks that are not in the Loop but that have at least one
 * predecessor inside the loop.
 */
UnorderedSet<cfg::Block*> Loop::get_exit_blocks() const {
  UnorderedSet<cfg::Block*> result;
  for (auto* block : m_blocks) {
    for (auto* edge : block->succs()) {
      if (!contains(edge->target())) {
        result.emplace(edge->target());
      }
    }
  }
  return result;
}

/**
 * Returns the blocks that are in the loop
 */
std::vector<cfg::Block*> Loop::get_blocks() const { return m_blocks; }

Loop::iterator Loop::begin() { return m_blocks.begin(); }

Loop::iterator Loop::end() { return m_blocks.end(); }

Loop::reverse_iterator Loop::rbegin() { return m_blocks.rbegin(); }

Loop::reverse_iterator Loop::rend() { return m_blocks.rend(); }

/*
 * Traverses the control flow graph and constructs a Loop for every strongly
 * connected component of its weak topological ordering, nested ones included.
 * See Loop for what holds of a loop that is not reducible.
 */
LoopInfo::LoopInfo(const cfg::ControlFlowGraph& cfg) {
  init(cfg, [](auto&, auto&, auto&, bool) { return nullptr; });
}

LoopInfo::LoopInfo(cfg::ControlFlowGraph& cfg, PreheaderTag) {
  init(cfg,
       [&](auto& cfg, const auto& block_set, auto loop_header,
           bool is_reducible) -> cfg::Block* {
         if (!is_reducible) {
           return nullptr;
         }
         auto loop_header_preds = loop_header->preds();
         auto loop_preheader = cfg.create_block();
         for (auto edge : loop_header_preds) {
           if (!block_set.count(edge->src())) {
             cfg.set_edge_target(edge, loop_preheader);
           }
         }

         // connect the preheader with the header
         auto* edge = new cfg::Edge(loop_preheader, loop_header,
                                    cfg::EdgeType::EDGE_GOTO);
         cfg.add_edge(edge);
         return loop_preheader;
       });
}

LoopInfo LoopInfo::make_with_preheaders(cfg::ControlFlowGraph& cfg) {
  return LoopInfo(cfg, PreheaderTag{});
}

template <typename Cfg, typename Fn>
void LoopInfo::init(Cfg& cfg, Fn preheader_fn) {
  sparta::WeakTopologicalOrdering<cfg::Block*> wto(
      cfg.entry_block(),
      [](const cfg::Block* block) -> std::vector<cfg::Block*> {
        std::vector<cfg::Block*> blocks;
        for (auto* edge : block->succs()) {
          blocks.emplace_back(edge->target());
        }
        return blocks;
      });

  // construct a level order traversal of a weak topological ordering
  std::vector<ComponentWrapper<cfg::Block*>> level_order;
  construct_level_order_traversal<cfg::Block*>(level_order, wto);

  // Mapping from all blocks that are loop headers and their respective Loop
  // object
  UnorderedMap<const cfg::Block*, Loop*> loop_heads;

  // Decided up front, on the CFG as given: make_with_preheaders inserts each
  // inner loop's preheader while going outward, and such a block is not part
  // of any enclosing component, so it would look like a second entry into it.
  std::vector<bool> reducible;
  reducible.reserve(level_order.size());
  for (auto it = level_order.rbegin(); it != level_order.rend(); ++it) {
    UnorderedSet<cfg::Block*> block_set;
    visit_depth_first<cfg::Block*>(
        it->get(), [&](cfg::Block* block) { block_set.emplace(block); });
    // Reducible iff no block but the head can be entered from outside. Then
    // every path into the loop comes in through the head, so the head
    // dominates the loop without a dominator tree being computed.
    auto* head = it->get().head_node();
    reducible.push_back(unordered_all_of(block_set, [&](cfg::Block* block) {
      return block == head ||
             std::all_of(block->preds().begin(), block->preds().end(),
                         [&](cfg::Edge* edge) {
                           return block_set.count(edge->src()) != 0u;
                         });
    }));
  }

  // Constructs a bare bones Loop for every SCC found by WTO, innermost first.
  size_t index = 0;
  for (auto it = level_order.rbegin(); it != level_order.rend();
       ++it, ++index) {
    std::vector<cfg::Block*> blocks_in_loop;
    UnorderedSet<cfg::Block*> block_set;
    UnorderedSet<Loop*> subloops;
    const auto& wto_comp = it->get();

    // construct blocks_in_loop, block_set, and subloops. Inner loops are
    // built first, so a nested loop that already has a parent is a deeper
    // descendant, not an immediate child.
    visit_depth_first<cfg::Block*>(wto_comp, [&](cfg::Block* block) {
      blocks_in_loop.emplace_back(block);
      block_set.emplace(block);
      auto head = loop_heads.find(block);
      if (head != loop_heads.end() &&
          head->second->get_parent_loop() == nullptr) {
        subloops.emplace(head->second);
      }
    });

    always_assert(!blocks_in_loop.empty());
    auto* loop_header = blocks_in_loop.front();
    bool is_reducible = reducible.at(index);
    auto loop_preheader =
        preheader_fn(cfg, block_set, loop_header, is_reducible);

    // we are traversing level_order backwards, so we insert in front to make it
    // level_order; also note that insertions into deques does not invalidate
    // references
    auto& loop = m_loops.emplace_front(blocks_in_loop, subloops, loop_preheader,
                                       is_reducible);

    for (auto* block : blocks_in_loop) {
      m_block_location.emplace(block, &loop);
    }

    loop_heads.emplace(loop_header, &loop);
  }
}

/**
 * Returns the innermost loop that contains block, or nullptr if block is not
 * contained in a loop
 */
Loop* LoopInfo::get_loop_for(cfg::Block* block) {
  auto it = m_block_location.find(block);
  return it != m_block_location.end() ? it->second : nullptr;
}

size_t LoopInfo::num_loops() { return m_loops.size(); }

LoopInfo::iterator LoopInfo::begin() { return m_loops.begin(); }

LoopInfo::iterator LoopInfo::end() { return m_loops.end(); }

LoopInfo::reverse_iterator LoopInfo::rbegin() { return m_loops.rbegin(); }

LoopInfo::reverse_iterator LoopInfo::rend() { return m_loops.rend(); }
