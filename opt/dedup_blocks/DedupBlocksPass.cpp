/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DedupBlocksPass.h"

#include <atomic>
#include <boost/optional.hpp>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRList.h"
#include "Resolver.h"
#include "Transform.h"
#include "Util.h"
#include "Walkers.h"

/*
 * This pass removes blocks that are duplicates in a method.
 *
 * If a method has multiple blocks with the same code and the same successors,
 * delete all but one of the blocks. Naming one of them the canonical block.
 *
 * Then, reroute all the predecessors of all the blocks to that canonical block.
 *
 * Merging these blocks will make some debug line numbers incorrect.
 * Here's an example
 *
 * Bar getBar() {
 *   if (condition1) {
 *     Bar bar = makeBar();
 *     if (condition2) {
 *       return bar;
 *     }
 *     cleanup();
 *   } else if (condition3) {
 *     cleanup();
 *   }
 *   return null;
 * }
 *
 * The blocks that call `cleanup()` will be merged.
 *
 * No matter which branch we took to call `cleanup()`, a stack trace will always
 * report the same line number (probably the first one in this example, because
 * it will have a lower block id).
 *
 * We could delete the line number information inside the canonical block, but
 * arguably, having stack traces that point to similar looking code (in a
 * different location) is better than having stack traces point to the nearest
 * line of source code before or after the merged block.
 *
 * Deleting the line info would also make things complicated if `cleanup()` is
 * inlined into `getBar()`. We would be unable to reconstruct the inlined stack
 * frame if we deleted the callsite's line number.
 */

namespace {

using hash_t = std::size_t;

// Structural equality of opcodes
static bool same_code(cfg::Block* b1, cfg::Block* b2) {
  return InstructionIterable(b1).structural_equals(InstructionIterable(b2));
}

// The blocks must also have the exact same successors
// (and reached in the same ways)
static bool same_successors(const cfg::Block* b1, const cfg::Block* b2) {
  const auto& b1_succs = b1->succs();
  const auto& b2_succs = b2->succs();
  if (b1_succs.size() != b2_succs.size()) {
    return false;
  }
  for (const cfg::Edge* b1_succ : b1_succs) {
    const auto& in_b2 = std::find_if(b2_succs.begin(), b2_succs.end(),
                                     [&b1_succ](const cfg::Edge* e) {
                                       return e->equals_ignore_source(*b1_succ);
                                     });
    if (in_b2 == b2_succs.end()) {
      // b1 has a succ that b2 doesn't. Note that both the succ blocks and
      // the edge types have to match
      return false;
    }
  }
  return true;
}

struct SuccBlocksInSameGroup {
  bool operator()(const cfg::Block* a, const cfg::Block* b) const {
    return same_successors(a, b) && a->same_try(b) &&
           a->is_catch() == b->is_catch();
  }
};

struct BlocksInSameGroup {
  bool operator()(cfg::Block* a, cfg::Block* b) const {
    return SuccBlocksInSameGroup{}(a, b) && same_code(a, b);
  }
};

struct BlockHasher {
  hash_t operator()(cfg::Block* b) const {
    hash_t result = 0;
    for (auto& mie : InstructionIterable(b)) {
      result ^= mie.insn->hash();
    }
    return result;
  }
};

struct BlockCompare {
  bool operator()(const cfg::Block* a, const cfg::Block* b) const {
    // This assumes that cfg::Block::operator<() is based on block ids
    return *a < *b;
  }
};

struct BlockSuccHasher {
  hash_t operator()(cfg::Block* b) const {
    hash_t result = 0;
    for (const auto& succ : b->succs()) {
      result ^= succ->target()->id();
    }
    return result;
  }
};

struct InstructionHasher {
  hash_t operator()(IRInstruction* insn) const { return insn->hash(); }
};

struct InstructionEquals {
  bool operator()(IRInstruction* a, IRInstruction* b) const { return *a == *b; }
};

// Choose an iteration order based on block ids for determinism. This returns a
// vector of pointers to the entries of the Map.
//
// * If the map is const, the vector has const pointers to the entries.
// * If the map is not const, the vector has non-const pointers to the entries.
//   * If you edit them, do so with extreme care. Changing the keys or the
//     results of the key hash/equality functions could be disastrous.
template <class UnorderedMap,
          class Entry =
              mimic_const_t<UnorderedMap, typename UnorderedMap::value_type>>
std::vector<Entry*> get_id_order(UnorderedMap& umap) {
  std::vector<Entry*> order;
  for (Entry& entry : umap) {
    order.push_back(&entry);
  }
  std::sort(order.begin(), order.end(),
            [](Entry* e1, Entry* e2) { return *(e1->first) < *(e2->first); });
  return order;
}

class DedupBlocksImpl {
 public:
  DedupBlocksImpl(const std::vector<DexClass*>& scope,
                  PassManager& mgr,
                  const DedupBlocksPass::Config& config)
      : m_scope(scope), m_mgr(mgr), m_config(config) {}

  // Dedup blocks that are exactly the same
  void dedup(cfg::ControlFlowGraph& cfg) {
    Duplicates dups = collect_duplicates(cfg);
    if (dups.size() > 0) {
      record_stats(dups);
      deduplicate(dups, cfg);
    }
  }

  /*
   * Split blocks that share postfix of instructions (that ends with the same
   * set of instructions).
   */
  void split_postfix(cfg::ControlFlowGraph& cfg) {
    PostfixSplitGroupMap dups = collect_postfix_duplicates(cfg);
    if (dups.size() > 0) {
      split_postfix_blocks(dups, cfg);
    }
  }

  void run() {
    walk::parallel::code(m_scope, [this](DexMethod* method, IRCode& code) {
      if (m_config.method_black_list.count(method) != 0) {
        return;
      }

      code.build_cfg(/* editable */ true);
      auto& cfg = code.cfg();

      if (m_config.split_postfix) {
        // TODO: Might want to do this repeatedly until no more work can be
        // performed, horribly inefficient though. We might want to do a proper
        // graph traversal in (forward/reverse) topological order.
        //
        // We could also split based on a shared prefix of instructions
        split_postfix(cfg);
      }

      dedup(cfg);

      code.clear_cfg();
    });
    report_stats();
  }

 private:
  // Be careful using `.at()` (or similar) on this map. We use a very broad
  // equality function that can lead to unexpected results. The key equality
  // function of this map is actually a check that they are duplicates, not that
  // they're the same block.
  //
  // Because `BlocksInSameGroup` depends on the CFG, modifications to the CFG
  // invalidate this map.
  using BlockSet = std::set<cfg::Block*, BlockCompare>;
  using Duplicates =
      std::unordered_map<cfg::Block*, BlockSet, BlockHasher, BlocksInSameGroup>;
  struct PostfixSplitGroup {
    BlockSet postfix_blocks;
    std::map<cfg::Block*, IRList::reverse_iterator, BlockCompare>
        postfix_block_its;
    size_t insn_count;
  };

  // Be careful using `.at()` on this map for the same reason as on `Duplicates`
  using PostfixSplitGroupMap = std::unordered_map<cfg::Block*,
                                                  PostfixSplitGroup,
                                                  BlockSuccHasher,
                                                  SuccBlocksInSameGroup>;
  const char* METRIC_BLOCKS_REMOVED = "blocks_removed";
  const char* METRIC_BLOCKS_SPLIT = "blocks_split";
  const char* METRIC_ELIGIBLE_BLOCKS = "eligible_blocks";
  const std::vector<DexClass*>& m_scope;
  PassManager& m_mgr;
  const DedupBlocksPass::Config& m_config;

  std::atomic_int m_num_eligible_blocks{0};
  std::atomic_int m_num_blocks_removed{0};
  std::atomic_int m_num_blocks_split{0};

  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> m_dup_sizes;
  std::mutex lock;

  // Find blocks with the same exact code
  Duplicates collect_duplicates(const cfg::ControlFlowGraph& cfg) {
    const auto& blocks = cfg.blocks();
    Duplicates duplicates;

    for (cfg::Block* block : blocks) {
      if (is_eligible(block)) {
        // Find a group that matches this one. The key equality function of this
        // map is actually a check that they are duplicates, not that they're
        // the same block.
        //
        // For example, if Block A and Block A' are duplicates, we will
        // populate this map as such:
        //   * after the first iteration (inserted A)
        //       A -> [A]
        //   * after the second iteration (inserted A')
        //       A -> [A, A']
        duplicates[block].insert(block);
        ++m_num_eligible_blocks;
      }
    }

    remove_singletons(duplicates,
                      [](auto it) { return it->second.size() <= 1; });
    return duplicates;
  }

  // Replace duplicated blocks with the "canon" (block with lowest ID).
  void dedup_blocks(cfg::ControlFlowGraph& cfg, const BlockSet& blocks) {
    // canon is block with lowest id.
    cfg::Block* canon = *blocks.begin();

    for (cfg::Block* block : blocks) {
      if (block != canon) {
        always_assert(canon->id() < block->id());

        cfg.replace_block(block, canon);
        ++m_num_blocks_removed;
      }
    }
  }

  // remove all but one of a duplicate set. Reroute the predecessors to the
  // canonical block
  void deduplicate(const Duplicates& dups, cfg::ControlFlowGraph& cfg) {
    fix_dex_pos_pointers(dups.begin(), dups.end(),
                         [](auto it) { return it->second; }, cfg);

    // Copy the BlockSets into a vector so that we're not reading the map while
    // editing the CFG.
    std::vector<BlockSet> order;
    for (const Duplicates::value_type* entry : get_id_order(dups)) {
      order.push_back(entry->second);
    }

    for (const BlockSet& group : order) {
      dedup_blocks(cfg, group);
    }
  }

  // The algorithm below identifies the best groups of blocks that share the
  // same postfix of instructions (ends with the same set of instructions),
  // as well as the best place to split the blocks so as to create new blocks
  // that are identical and can be dedup with the subsequent dedup process.
  //
  // The highlevel flow of the algorithm works as follows:
  // 1. Partition blocks into groups that share the same successors. Note the
  // current implementation assumes one successor but we can easily improve
  // the partition algorithm to partition for same *set* of sucessor.
  // 2. For each block group that share the same successors, start the
  // instruction comparing process by keeping a reverse iterator for each block
  // within the group.
  // 3. In each iteration, partition the groups further by comparing the exact
  // instruction at the current reverse iterator - that is, the ones with the
  // same instruction at the current location share the same group, and we keep
  // a count of how many blocks are within the same group.
  // 4. Now that we have the groups, pick the biggest group. This means you are
  // effectively being greedy and choose the one that achieves the most sharing
  // in current level. The rest of the groups are discarded. In the future,
  // we can consider keeping all groups and simply eliminate the groups that
  // no longer share any instructions from further iteration, but don't throw
  // away those eliminated groups - we can still split them later (as opposed to
  // only the potentially best group).
  // 5. However, note that being greedy isn't necessarily the best choice, we
  // calculate the potential savings by calculating the "rectangle" of current
  // instruction "depth" * (number of blocks - 1), and keep track the best we
  // have seen so far, including the blocks and the reverse iterators.
  // 6. Keep iterating the groups that are being tracked until there are no more
  // sharing can be achieved.
  // 7. Split the blocks as indicated by the reverse iterator based on the best
  // saving we've seen so far.
  //
  // Example:
  //
  // Assuming you start with 5 groups (instructions are simplified for brievity
  // purposes)
  // A: (add, const v0, const v1, add, add, add)
  // B: (mul, const v0, const v1, add, add, add)
  // C: (div, const v0, const v1, add, add, add)
  // D: (const v2, add, add)
  // E: (const v3, add, add)
  //
  // All of them have the same successor.
  // 1. Start with (A, B, C, D, E) in the same group as they share the same
  // successor.
  // 2. Reverse iterator of A, B, C, D, E are at (5, 5, 5, 2, 2) (index starting
  // from 0).
  // 3. Iteration #1: Looking at the add instruction, given that all the
  // iterators pointing to identical add instruction, the group is still
  // (A, B, C, D, E), and the iterators are (4, 4, 4, 1, 1). The current saving
  // is 1 * (5-1) = 4.
  // 4. Iteration #2: Still the same add instruction. The group is still
  // (A, B, C, D, E), and the iterators are (3, 3, 3, 0, 0). The current saving
  // is 2 * (5-1) = 8.
  // 5. Iteration #3: group (A, B, C) share the same (add) instruction,
  // while (D), (E) are their own groups. (A, B, C) gets selected and rest is
  // discarded. The current saving is 3 * (3-1) = 6.
  // 6. Iteration #4: group (A, B, C) share the same (const v1) instruction.
  // The current saving is 4 * (3-1) = 8.
  // 7. Iteration #5: group (A, B, C) share the same (const v0) instruction.
  // The current saving is 5 * (3-1) = 10.
  // 8. Iteration #6. group (A), (B), (C) are their own groups since they all
  // have unique instruction. Given the biggest group is size 1, we terminate
  // the algorithm. The best saving is 10 with group (A, B, C) and split at
  // (1, 1, 1).
  // @TODO - Instead of keeping track of just one group, in the future we can
  // consider maintaining multiple groups and split them.
  PostfixSplitGroupMap collect_postfix_duplicates(
      const cfg::ControlFlowGraph& cfg) {
    const auto& blocks = cfg.blocks();
    PostfixSplitGroupMap splitGroupMap;

    // Group by successors if blocks share a single successor block.
    for (cfg::Block* block : blocks) {
      if (is_eligible(block)) {
        if (block->num_opcodes() >= m_config.block_split_min_opcode_count) {
          // Insert into other blocks that share the same successors
          splitGroupMap[block].postfix_blocks.insert(block);
        }
      }
    }

    TRACE(DEDUP_BLOCKS, 4,
          "split_postfix: partitioned %d blocks into %d groups\n",
          blocks.size(), splitGroupMap.size());

    struct CountGroup {
      size_t count = 0;
      BlockSet blocks;
    };

    // For each ([succs], [blocks]) pair
    for (PostfixSplitGroupMap::value_type* entry :
         get_id_order(splitGroupMap)) {
      const cfg::Block* b = entry->first;
      auto& split_group = entry->second;
      auto& succ_blocks = split_group.postfix_blocks;
      if (succ_blocks.size() <= 1) {
        continue;
      }

      TRACE(DEDUP_BLOCKS, 4,
            "split_postfix: current group (succs=%d, blocks=%d)\n",
            b->succs().size(), succ_blocks.size());

      // Keep track of best we've seen so far.
      BlockSet best_blocks;
      std::map<cfg::Block*, IRList::reverse_iterator, BlockCompare>
          best_block_its;
      size_t best_insn_count = 0;
      size_t best_saved_insn = 0;

      // Get (reverse) iterators for all blocks.
      std::map<cfg::Block*, IRList::reverse_iterator, BlockCompare>
          block_iterator_map;
      for (auto block : succ_blocks) {
        block_iterator_map[block] = block->rbegin();
      }

      // Find the best common blocks
      size_t cur_insn_index = 0;
      while (true) {
        TRACE(DEDUP_BLOCKS, 4, "split_postfix: scanning instruction at %d\n",
              cur_insn_index);

        // For each "iteration" - we count the distinct instructions and select
        // the instruction with highest count - the majority.
        // We do remember the instructions saved and select the best
        // combination at the end.
        size_t majority = 0;
        IRInstruction* majority_insn = nullptr;

        // For each (Block, iterator) - advance the iterator and partition
        // the current set of blocks into two groups:
        // 1) the group with the most shared instructions (majority).
        // 2) the rest.
        // The following insn_count map maintains the (insn -> count) mapping
        // so that we can group the blocks based on the current instruction.
        // For example, if you have A, B, C, D, E blocks, and (A, B, C) share
        // the same instruction I1, while (D, E) share the same instruction I2,
        // you would end up with (I1 : (A, B, C), 3, and I2 : (D, E), 2).
        // With the above map, you can select I1 group (A, B, C) as the current
        // group to track (current implementation).
        // @TODO - Instead of only keeping one group and calculate best savings
        // based on just one group, maintain multiple groups at the same time
        // and split/dedup those groups.
        std::unordered_map<IRInstruction*, CountGroup, InstructionHasher,
                           InstructionEquals>
            insn_count;

        for (auto& block_iterator_pair : block_iterator_map) {
          const auto block = block_iterator_pair.first;
          auto& it = block_iterator_pair.second;

          // Skip all non-instructions.
          while (it != block->rend() && it->type != MFLOW_OPCODE) {
            ++it;
          }

          if (it != block->rend()) {
            // Count the instructions and locate the majority
            auto& count_group = insn_count[it->insn];
            count_group.count++;
            count_group.blocks.insert(block);
            if (count_group.count > majority) {
              majority = count_group.count;
              majority_insn = it->insn;
            }

            // Move to next instruction.
            // IMPORTANT: we should always land on instructions otherwise
            // you can get subtle errors when converting between different
            // instruction iterators.
            do {
              ++it;
            } while (it != block->rend() && it->type != MFLOW_OPCODE);
          }
        }

        // No group to count or no one group has more than 1 item in common.
        // In either case we are done.
        if (majority_insn == nullptr || majority <= 1) {
          break;
        }

        cur_insn_index++;
        auto& majority_count_group = insn_count[majority_insn];

        // Remove the iterators
        for (auto it = block_iterator_map.begin();
             it != block_iterator_map.end();) {
          if (majority_count_group.blocks.find(it->first) ==
              majority_count_group.blocks.end()) {
            // Remove iterator that is not in the majority group
            it = block_iterator_map.erase(it);
          } else {
            it++;
          }
        }

        // Is this the best saving we've seen so far?
        // Note we only want at least 3 level deep otherwise it is probably not
        // quite worth it (configurable).
        size_t cur_saved_insn =
            cur_insn_index * (majority_count_group.blocks.size() - 1);
        if (cur_saved_insn > best_saved_insn &&
            cur_insn_index >= m_config.block_split_min_opcode_count) {
          // Save it
          best_saved_insn = cur_saved_insn;
          best_insn_count = cur_insn_index;
          best_block_its = block_iterator_map;
          best_blocks = std::move(majority_count_group.blocks);
        }
      }

      // Update the current group with the best savings
      TRACE(DEDUP_BLOCKS, 4,
            "split_postfix: best block group.size() = %d, instruction at %d\n",
            best_blocks.size(), best_insn_count);
      split_group.postfix_block_its = std::move(best_block_its);
      split_group.postfix_blocks = std::move(best_blocks);
      split_group.insn_count = best_insn_count;
    }

    remove_singletons(splitGroupMap, [](auto it) {
      return it->second.postfix_blocks.size() <= 1;
    });

    TRACE(DEDUP_BLOCKS, 4, "split_postfix: total split groups = %d\n",
          splitGroupMap.size());
    return splitGroupMap;
  }

  // For each group, split the blocks in the group where the reverse iterator
  // is located and dedup the common block created from split.
  void split_postfix_blocks(const PostfixSplitGroupMap& dups,
                            cfg::ControlFlowGraph& cfg) {
    fix_dex_pos_pointers(dups.begin(), dups.end(),
                         [](auto it) { return it->second.postfix_blocks; },
                         cfg);

    for (const PostfixSplitGroupMap::value_type* entry : get_id_order(dups)) {
      const auto& group = entry->second;
      TRACE(DEDUP_BLOCKS, 4,
            "split_postfix: splitting blocks.size() = %d, instruction at %d\n",
            group.postfix_blocks.size(), group.insn_count);

      // Split the blocks at the reverse iterator where we determine to be
      // the best location.
      for (const auto& block_it_pair : group.postfix_block_its) {
        auto block = block_it_pair.first;
        auto it = block_it_pair.second;

        // This means we are to split the entire block, which is essentially a
        // no-op and will be handled by dedup later.
        if (it == block->rend()) {
          continue;
        }

        // To convert reverse_iterator to iterator, we need to call .base() but
        // that points to the previous item. So we need to account for that.
        auto fwd_it =
            ir_list::InstructionIterator(std::prev(it.base()), block->end());
        auto fwd_it_end = ir_list::InstructionIterable(*block).end();

        // If we are splitting at the boundary of following iget/sget/invoke/
        // filled-new-array we should skip to the next instruction. Otherwise
        // splitting would generate a goto in between and lead to invalid
        // instruction.
        while (fwd_it != fwd_it_end) {
          auto fwd_it_next = fwd_it;
          fwd_it_next++;
          if (fwd_it_next != fwd_it_end) {
            auto opcode = fwd_it_next->insn->opcode();
            if (opcode::is_move_result_or_move_result_pseudo(opcode)) {
              fwd_it = fwd_it_next;
              continue;
            }
          }

          break;
        }

        if (fwd_it == fwd_it_end || fwd_it.unwrap() == block->get_last_insn()) {
          continue;
        }

        // Split the block
        auto split_block =
            cfg.split_block(block->to_cfg_instruction_iterator(fwd_it));
        TRACE(DEDUP_BLOCKS, 4,
              "split_postfix: split block : old = %d, new = %d\n", block->id(),
              split_block->id());
        ++m_num_blocks_split;
      }
    }
  }

  // DexPositions have `parent` pointers to other DexPositions inside the same
  // method. We will delete some of these DexPositions, which would create
  // dangling pointers.
  //
  // This method changes those parent pointers to the equivalent DexPosition
  // in the canonical block
  template <typename IteratorType, typename GetBlock>
  void fix_dex_pos_pointers(IteratorType begin,
                            IteratorType end,
                            GetBlock get_blocks,
                            cfg::ControlFlowGraph& cfg) {
    // A map from the DexPositions we're about to delete to the equivalent
    // DexPosition in the canonical block.
    std::unordered_map<DexPosition*, DexPosition*> position_replace_map;

    for (IteratorType it = begin; it != end; ++it) {
      const auto& blocks = get_blocks(it);

      // canon is block with lowest id.
      cfg::Block* canon = *blocks.begin();

      std::vector<DexPosition*> canon_positions;
      for (auto& mie : *canon) {
        if (mie.type == MFLOW_POSITION) {
          canon_positions.push_back(mie.pos.get());
        }
      }

      for (cfg::Block* block : blocks) {
        if (block != canon) {
          // All of `block`s positions are about to be deleted. Add the mapping
          // from this position to the equivalent canonical position.
          size_t i = 0;
          for (auto& mie : *block) {
            if (mie.type == MFLOW_POSITION) {
              // If canon has no DexPositions, clear out parent pointers
              auto replacement =
                  !canon_positions.empty() ? canon_positions.at(i) : nullptr;
              position_replace_map.emplace(mie.pos.get(), replacement);

              ++i;
              if (i >= canon_positions.size()) {
                // block has more DexPositions than canon.
                // keep re-using the last one, I guess? FIXME
                //
                // TODO: Maybe we could associate DexPositions with their
                // closest IRInstruction, then combine the DexPositions that
                // share the same deduped IRInstruction.
                i = canon_positions.size() - 1;
              }
            }
          }
        }
      }
    }

    // Search for dangling parent pointers and replace them
    for (cfg::Block* b : cfg.blocks()) {
      for (auto& mie : *b) {
        if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr) {
          auto it = position_replace_map.find(mie.pos->parent);
          if (it != position_replace_map.end()) {
            mie.pos->parent = it->second;
          }
        }
      }
    }
  }

  static bool is_eligible(cfg::Block* block) {
    if (!has_opcodes(block)) {
      return false;
    }

    // We can't split up move-result(-pseudo) instruction pairs
    if (begins_with_move_result(block)) {
      return false;
    }

    if (constructs_object_from_another_block(block)) {
      return false;
    }

    // TODO: It's not worth the goto to merge return-only blocks. What size is
    // the minimum?

    return true;
  }

  static bool begins_with_move_result(cfg::Block* block) {
    const auto& first_mie = *block->get_first_insn();
    auto first_op = first_mie.insn->opcode();
    return is_move_result(first_op) || opcode::is_move_result_pseudo(first_op);
  }

  // Deal with a verification error like this
  //
  // A: new-instance v0
  //    add-int v1, v2, v3       (this is here to clarify that A != C)
  // B: v0 <init>
  //
  //    ...
  //
  // C: new-instance v0
  // D: v0 <init>
  //
  // B == D. Coalesce!
  //
  // A: new-instance v0
  //    add-int v1, v2, v3
  // B: v0 <init>
  //
  // C: new-instance v0
  //    goto B
  //
  // But the verifier doesn't like this. When it merges v0 on B,
  // it declares it to be a conflict because they were instantiated
  // on different lines.
  // See androidxref.com/6.0.1_r10/xref/art/runtime/verifier/reg_type.cc#684
  //
  // It would be impossible to write this in java, but if you tried it would
  // look like this
  //
  // if (someCondition) {
  //   Foo a;
  // } else {
  //   Foo b;
  // }
  // (a or b) = new Foo();
  //
  // We try to avoid this situation by skipping the blocks with uninitalized
  // objects at the entrance
  static bool constructs_object_from_another_block(cfg::Block* block) {
    std::unordered_set<uint16_t> objs;

    const auto& iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      if (is_new_instance(it->insn->opcode())) {
        ++it;
        if (it == iterable.end()) {
          break;
        }
        always_assert(it->type == MFLOW_OPCODE &&
                      opcode::is_move_result_pseudo(it->insn->opcode()));
        objs.insert(it->insn->dest());

      } else if (is_invoke_direct(it->insn->opcode())) {
        if (is_init(it->insn->get_method())) {
          auto reg = it->insn->src(0);
          if (objs.count(reg) == 0) {
            // Found a constructor call on an object from another block
            return true;
          } else {
            // normal initialization
            objs.erase(reg);
          }
        }
      }
    }
    return false;
  }

  void record_stats(const Duplicates& duplicates) {
    // avoid the expensive lock if we won't actually print the information
    if (traceEnabled(DEDUP_BLOCKS, 2)) {
      std::lock_guard<std::mutex> guard{lock};
      for (const auto& entry : duplicates) {
        const auto& blocks = entry.second;
        // all blocks have the same number of opcodes
        cfg::Block* block = *blocks.begin();
        m_dup_sizes[num_opcodes(block)] += blocks.size();
      }
    }
  }

  void report_stats() {
    int eligible_blocks = m_num_eligible_blocks.load();
    int removed = m_num_blocks_removed.load();
    int split = m_num_blocks_split.load();
    m_mgr.incr_metric(METRIC_ELIGIBLE_BLOCKS, eligible_blocks);
    m_mgr.incr_metric(METRIC_BLOCKS_REMOVED, removed);
    m_mgr.incr_metric(METRIC_BLOCKS_SPLIT, split);
    TRACE(DEDUP_BLOCKS, 2, "%d eligible_blocks\n", eligible_blocks);

    for (const auto& entry : m_dup_sizes) {
      TRACE(DEDUP_BLOCKS,
            2,
            "found %d duplicate blocks with %d instructions\n",
            entry.second,
            entry.first);
    }

    TRACE(DEDUP_BLOCKS, 1, "%d blocks split\n", split);
    TRACE(DEDUP_BLOCKS, 1, "%d blocks removed\n", removed);
  }

  // remove sets with only one block
  template <typename TKey,
            typename TValue,
            typename THash,
            typename TPred,
            typename NeedToRemove>
  static void remove_singletons(
      std::unordered_map<TKey, TValue, THash, TPred>& duplicates,
      NeedToRemove need_to_remove) {
    for (auto it = duplicates.begin(); it != duplicates.end();) {
      if (need_to_remove(it)) {
        it = duplicates.erase(it);
      } else {
        ++it;
      }
    }
  }

  static boost::optional<MethodItemEntry&> last_opcode(cfg::Block* block) {
    for (auto it = block->rbegin(); it != block->rend(); it++) {
      if (it->type == MFLOW_OPCODE) {
        return *it;
      }
    }
    return boost::none;
  }

  static bool has_opcodes(cfg::Block* block) {
    const auto& iterable = InstructionIterable(block);
    return !iterable.empty();
  }

  static size_t num_opcodes(cfg::Block* block) {
    size_t result = 0;
    const auto& iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      result++;
    }
    return result;
  }

  static void print_dups(Duplicates dups) {
    TRACE(DEDUP_BLOCKS, 4, "duplicate blocks set: {\n");
    for (const auto& entry : dups) {
      TRACE(DEDUP_BLOCKS, 4, "  hash = %lu\n", BlockHasher{}(entry.first));
      for (cfg::Block* b : entry.second) {
        TRACE(DEDUP_BLOCKS, 4, "    block %d\n", b->id());
        for (const MethodItemEntry& mie : *b) {
          TRACE(DEDUP_BLOCKS, 4, "      %s\n", SHOW(mie));
        }
      }
    }
    TRACE(DEDUP_BLOCKS, 4, "} end duplicate blocks set\n");
  }
};

} // namespace

void DedupBlocksPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  DedupBlocksImpl impl(scope, mgr, m_config);
  impl.run();
}

static DedupBlocksPass s_pass;
