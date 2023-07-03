/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This service removes blocks that are duplicates in a method.
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

#include "DedupBlocks.h"
#include "DedupBlockValueNumbering.h"

#include <algorithm>

#include "DexPosition.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "OutlinedMethods.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Resolver.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "TypeInference.h"
#include <boost/functional/hash.hpp>

namespace {

constexpr bool kDebugForceInstrumentMode = false;

using hash_t = std::size_t;

static bool is_branch_or_goto(const cfg::Edge* edge) {
  auto type = edge->type();
  return type == cfg::EDGE_BRANCH || type == cfg::EDGE_GOTO;
}

static std::vector<cfg::Edge*> get_branch_or_goto_succs(
    const cfg::Block* block) {
  std::vector<cfg::Edge*> succs;
  for (auto edge : block->succs()) {
    if (is_branch_or_goto(edge)) {
      succs.push_back(edge);
    }
  }
  return succs;
}

// The blocks must also have the exact same branch and goto successors.
static bool same_branch_and_goto_successors(const cfg::Block* b1,
                                            const cfg::Block* b2) {
  auto b1_succs = get_branch_or_goto_succs(b1);
  auto b2_succs = get_branch_or_goto_succs(b2);
  if (b1_succs.size() != b2_succs.size()) {
    return false;
  }
  using Key = std::pair<cfg::EdgeType, cfg::Edge::CaseKey>;
  std::unordered_map<Key, cfg::Block*, boost::hash<Key>> b2_succs_map;
  for (auto b2_succ : b2_succs) {
    b2_succs_map.emplace(
        std::make_pair(b2_succ->type(), b2_succ->case_key().value_or(0)),
        b2_succ->target());
  }
  for (auto b1_succ : b1_succs) {
    // For successors being the same, we need to find a matching entry for
    // b1_succ in the b2_succs_map map.
    auto it = b2_succs_map.find(
        std::make_pair(b1_succ->type(), b1_succ->case_key().value_or(0)));
    if (it == b2_succs_map.end()) {
      return false;
    }
    auto b2_succ_target = it->second;
    // Either targets need to be the same, or both targets must be pointing to
    // same block (to support deduping of simple self-loops).
    if (b1_succ->target() != b2_succ_target &&
        (b1_succ->target() != b1 || b2_succ_target != b2)) {
      return false;
    }
  }
  return true;
}

struct SuccBlocksInSameGroup {
  bool operator()(const cfg::Block* a, const cfg::Block* b) const {
    return same_branch_and_goto_successors(a, b) && a->same_try(b) &&
           a->is_catch() == b->is_catch();
  }
};

struct BlockAndBlockValuePair {
  cfg::Block* block;
  const DedupBlkValueNumbering::BlockValue* block_value;
};

const cfg::Block& operator*(const BlockAndBlockValuePair& p) {
  return *p.block;
}

struct BlockAndBlockValuePairInSameGroup {
  bool operator()(const BlockAndBlockValuePair& p,
                  const BlockAndBlockValuePair& q) const {
    return SuccBlocksInSameGroup{}(p.block, q.block) &&
           *p.block_value == *q.block_value;
  }
};

struct BlockAndBlockValuePairHasher {
  hash_t operator()(const BlockAndBlockValuePair& p) const {
    return DedupBlkValueNumbering::BlockValueHasher()(*p.block_value);
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
      if (is_branch_or_goto(succ) && b == succ->target()) {
        result ^= 27277 * (hash_t)succ->type();
      } else {
        result ^= succ->target()->id();
      }
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
  order.reserve(umap.size());
  for (Entry& entry : umap) {
    order.push_back(&entry);
  }
  std::sort(order.begin(), order.end(),
            [](Entry* e1, Entry* e2) { return *(e1->first) < *(e2->first); });
  return order;
}

// Will the split block have a position before the first
// instruction, or do we need to insert one?
bool needs_pos(const IRList::iterator& begin, const IRList::iterator& end) {
  for (auto it = begin; it != end; it++) {
    switch (it->type) {
    case MFLOW_OPCODE: {
      auto op = it->insn->opcode();
      if (opcode::may_throw(op) || opcode::is_throw(op)) {
        return true;
      }
      continue;
    }
    case MFLOW_POSITION:
      return false;
    default:
      continue;
    }
  }
  return true;
}

} // namespace

namespace dedup_blocks_impl {

bool is_ineligible_because_of_fill_in_stack_trace(const IRInstruction* insn) {
  auto op = insn->opcode();
  // Direct call to a constructor whose class derives from Throwable?
  // (It would indirectly call java.lang.Throwable.fillInStackTrace.)
  if (opcode::is_invoke_direct(op) && method::is_init(insn->get_method()) &&
      type::is_subclass(type::java_lang_Throwable(),
                        insn->get_method()->get_class())) {
    return true;
  }
  // Explicit virtual call to the java.lang.Throwable.fillInStackTrace
  // method?
  if (opcode::is_invoke_virtual(op) &&
      resolve_method(insn->get_method(), MethodSearch::Virtual) ==
          method::java_lang_Throwable_fillInStackTrace()) {
    return true;
  }
  // An outlined method might invoke one of the above, so we are being
  // conservative here.
  if (opcode::is_invoke_static(op) &&
      outliner::is_outlined_method(insn->get_method())) {
    return true;
  }
  return false;
}

class DedupBlocksImpl {
 public:
  DedupBlocksImpl(const Config* config, Stats& stats)
      : m_config(config), m_stats(stats) {
    always_assert(m_config);
  }

  // Dedup blocks that are exactly the same
  bool dedup(bool is_static,
             DexType* declaring_type,
             DexTypeList* args,
             cfg::ControlFlowGraph& cfg) {
    cfg.calculate_exit_block();
    LivenessFixpointIterator liveness_fixpoint_iter(cfg);
    liveness_fixpoint_iter.run({});
    DedupBlkValueNumbering::BlockValues block_values(liveness_fixpoint_iter);
    Duplicates dups = collect_duplicates(is_static, declaring_type, args, cfg,
                                         block_values, liveness_fixpoint_iter);
    if (!dups.empty()) {
      if (m_config->debug) {
        check_inits(cfg);
      }
      record_stats(dups);
      bool res = deduplicate(dups, cfg);
      if (m_config->debug) {
        check_inits(cfg);
      }
      return res;
    }

    return false;
  }

  /*
   * Split blocks that share postfix of instructions (that ends with the same
   * set of instructions).
   *
   * TODO: Some split blocks might not actually get dedup, as deduping checks
   * additional type constraints that splitting ignores. Either perfectly line
   * line up the splitting logic with the deduping logic (and install asserts
   * that they agree), or re-merge left-over block pairs, either explicitly,
   * or by running another RemoveGotos pass.
   *
   * TODO(T132664146): DedupBlocks's splitting algorithm unfortunately only
   * splits out one subtree level per iteration, which could probably be
   * improved with a better algorithm.
   */
  void split_postfix(cfg::ControlFlowGraph& cfg) {
    PostfixSplitGroupMap dups = collect_postfix_duplicates(cfg);
    if (!dups.empty()) {
      if (m_config->debug) {
        check_inits(cfg);
      }
      split_postfix_blocks(dups, cfg);
      if (m_config->debug) {
        check_inits(cfg);
      }
    }
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
  using Duplicates = std::unordered_map<BlockAndBlockValuePair,
                                        BlockSet,
                                        BlockAndBlockValuePairHasher,
                                        BlockAndBlockValuePairInSameGroup>;
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
  const Config* m_config;
  Stats& m_stats;

  struct LiveRanges {
    live_range::MoveAwareChains chains;
    Lazy<live_range::DefUseChains> def_use_chains;
    Lazy<live_range::UseDefChains> use_def_chains;
    explicit LiveRanges(cfg::ControlFlowGraph& cfg)
        : chains(cfg),
          def_use_chains([this] { return chains.get_def_use_chains(); }),
          use_def_chains([this] { return chains.get_use_def_chains(); }) {}
  };

  // Find blocks with the same exact code
  Duplicates collect_duplicates(
      bool is_static,
      DexType* declaring_type,
      DexTypeList* args,
      cfg::ControlFlowGraph& cfg,
      DedupBlkValueNumbering::BlockValues& block_values,
      LivenessFixpointIterator& liveness_fixpoint_iter) {
    const auto& blocks = cfg.blocks();
    Duplicates duplicates;

    Lazy<LiveRanges> live_ranges(
        [&]() { return std::make_unique<LiveRanges>(cfg); });

    for (cfg::Block* block : blocks) {
      if (is_eligible(block, cfg)) {
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
        auto& dups = duplicates[{block, block_values.get_block_value(block)}];
        dups.insert(block);
        ++m_stats.eligible_blocks;
      }
    }

    Lazy<type_inference::TypeInference> type_inference([&]() {
      auto res = std::make_unique<type_inference::TypeInference>(cfg);
      res->run(is_static, declaring_type, args);
      return res;
    });
    remove_if(duplicates, [&](auto& blocks) {
      return is_singleton_or_inconsistent(
          blocks, live_ranges, liveness_fixpoint_iter, type_inference);
    });
    return duplicates;
  }

  static size_t remove_instructions(cfg::Block* block,
                                    const cfg::ControlFlowGraph& cfg) {
    size_t cnt{0};

    auto it = block->begin();
    while (it != block->end()) {
      auto cur_it = it;
      ++it;

      switch (cur_it->type) {
      // Remove.
      case MFLOW_OPCODE: {
        block->remove_mie(cur_it);
        ++cnt;
      } break;

      // Keep.
      case MFLOW_SOURCE_BLOCK:
      case MFLOW_POSITION:
      case MFLOW_DEBUG:
        break;

      // Shouldn't be here.
      case MFLOW_TARGET:
      case MFLOW_CATCH:
      case MFLOW_TRY:
      case MFLOW_DEX_OPCODE:
      case MFLOW_FALLTHROUGH:
        always_assert_log(false, "Found unsupported mie %s\n%s", SHOW(*cur_it),
                          SHOW(cfg));
      }
    }

    return cnt;
  }

  // remove all but one of a duplicate set. Reroute the predecessors to the
  // canonical block
  bool deduplicate(const Duplicates& dups, cfg::ControlFlowGraph& cfg) {
    // Copy the BlockSets into a vector so that we're not reading the map while
    // editing the CFG.
    std::vector<BlockSet> order;
    for (const Duplicates::value_type* entry : get_id_order(dups)) {
      order.push_back(entry->second);
    }

    size_t cnt{0}; // Removed blocks.

    if (g_redex->instrument_mode || kDebugForceInstrumentMode) {
      for (const BlockSet& group : order) {
        // canon is block with lowest id.
        cfg::Block* canon = *group.begin();

        for (cfg::Block* block : group) {
          if (block == canon) {
            continue;
          }

          always_assert(canon->id() < block->id());

          // If there is an incoming exception edge, forwarding does *not*
          // work. This should have been filtered before.
          auto exc_check_fn = [&]() {
            if (cfg.get_pred_edge_of_type(block, cfg::EdgeType::EDGE_THROW) ==
                nullptr) {
              return true;
            }
            auto it = block->get_first_insn();
            if (it == block->end()) {
              return true;
            }
            return !opcode::is_move_exception(it->insn->opcode());
          };
          redex_assert(exc_check_fn());

          // Don't remove directly. Just remove everything but source blocks
          // and dex positions. Then remove all outgoing edges and make a goto
          // to the replacement.
          //
          // This will overcount source blocks, and not deal correctly with
          // exceptions.
          //
          // TODO: Consider splitting all in-edges of the canonical block and
          //       adding its source blocks there. This will still not deal
          //       correctly with exception edges, but fix counting.

          cfg.delete_edges(block->succs().begin(), block->succs().end());

          // Undercounts branch instructions.
          m_stats.insns_removed += remove_instructions(block, cfg);

          cfg.add_edge(block, canon, cfg::EDGE_GOTO);

          ++cnt;
        }
      }
      cfg.simplify();
    } else {
      // Replace duplicated blocks with the "canon" (block with lowest ID).
      std::vector<std::pair<cfg::Block*, cfg::Block*>> blocks_to_replace;
      for (const BlockSet& group : order) {
        // canon is block with lowest id.
        cfg::Block* canon = *group.begin();

        for (cfg::Block* block : group) {
          if (block != canon) {
            always_assert(canon->id() < block->id());

            blocks_to_replace.emplace_back(block, canon);
            ++cnt;
          }
        }
      }

      // Note that replace_blocks also fixes any arising dangling parents.
      m_stats.insns_removed += cfg.replace_blocks(blocks_to_replace);
    }

    m_stats.blocks_removed += cnt;

    return cnt > 0;
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
  PostfixSplitGroupMap collect_postfix_duplicates(cfg::ControlFlowGraph& cfg) {
    const auto& blocks = cfg.blocks();
    PostfixSplitGroupMap splitGroupMap;

    // Group by successors if blocks share a single successor block.
    for (cfg::Block* block : blocks) {
      if (block->num_opcodes() >= m_config->block_split_min_opcode_count) {
        // Insert into other blocks that share the same successors
        splitGroupMap[block].postfix_blocks.insert(block);
      }
    }

    TRACE(DEDUP_BLOCKS, 4,
          "split_postfix: partitioned %zu blocks into %zu groups",
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
            "split_postfix: current group (succs=%zu, blocks=%zu)",
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
        TRACE(DEDUP_BLOCKS, 4, "split_postfix: scanning instruction at %zu",
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
        std20::erase_if(block_iterator_map, [&](auto& p) {
          return majority_count_group.blocks.find(p.first) ==
                 majority_count_group.blocks.end();
        });

        // Is this the best saving we've seen so far?
        // Note we only want at least block_split_min_opcode_count deep (config)
        size_t cur_saved_insn =
            cur_insn_index * (majority_count_group.blocks.size() - 1);
        if (cur_saved_insn >= best_saved_insn &&
            cur_insn_index >= m_config->block_split_min_opcode_count) {
          // Save it
          best_saved_insn = cur_saved_insn;
          best_insn_count = cur_insn_index;
          best_block_its = block_iterator_map;
          best_blocks = std::move(majority_count_group.blocks);
        }
      }

      // Update the current group with the best savings
      TRACE(DEDUP_BLOCKS, 4,
            "split_postfix: best block group.size() = %zu, instruction at %zu",
            best_blocks.size(), best_insn_count);
      split_group.postfix_block_its = std::move(best_block_its);
      split_group.postfix_blocks = std::move(best_blocks);
      split_group.insn_count = best_insn_count;
    }

    remove_if(splitGroupMap,
              [&](auto& entry) { return entry.postfix_blocks.size() <= 1; });

    TRACE(DEDUP_BLOCKS, 4, "split_postfix: total split groups = %zu",
          splitGroupMap.size());
    return splitGroupMap;
  }

  // For each group, split the blocks in the group where the reverse iterator
  // is located and dedup the common block created from split.
  void split_postfix_blocks(const PostfixSplitGroupMap& dups,
                            cfg::ControlFlowGraph& cfg) {
    for (const PostfixSplitGroupMap::value_type* entry : get_id_order(dups)) {
      const auto& group = entry->second;
      TRACE(DEDUP_BLOCKS, 4,
            "split_postfix: splitting blocks.size() = %zu, instruction at %zu",
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
            if (opcode::is_move_result_any(opcode)) {
              fwd_it = fwd_it_next;
              continue;
            }
          }

          break;
        }

        if (fwd_it == fwd_it_end || fwd_it.unwrap() == block->get_last_insn()) {
          continue;
        }

        auto cfg_it = block->to_cfg_instruction_iterator(fwd_it);
        // Split the block
        auto split_block = cfg.split_block(cfg_it);

        TRACE(DEDUP_BLOCKS, 4,
              "split_postfix: split block : old = %zu, new = %zu", block->id(),
              split_block->id());

        // Position of first instruction of split-off successor block
        if (needs_pos(split_block->begin(), split_block->end())) {
          auto pos = cfg.get_dbg_pos(cfg_it);
          if (pos) {
            // Make sure new block gets proper position
            cfg.insert_before(split_block, split_block->begin(),
                              std::make_unique<DexPosition>(*pos));
            ++m_stats.positions_inserted;
          }
        }

        ++m_stats.blocks_split;
      }
    }
  }

  bool is_eligible(cfg::Block* block, cfg::ControlFlowGraph& cfg) {
    // We can't split up move-result(-pseudo) instruction pairs
    if (begins_with_move_result(block)) {
      return false;
    }

    // For debugability, we don't want to dedup blocks involved direct or
    // indirect calls to Throwable.fillInStackTrace
    if (is_ineligible_because_of_fill_in_stack_trace(block)) {
      return false;
    }

    // Empty blocks are possibly necessary for profiling markers, or will
    // be cleaned up by CFG deconstruction.
    if ((g_redex->instrument_mode || kDebugForceInstrumentMode) &&
        block->get_first_insn() == block->end()) {
      return false;
    }

    // When instrumenting, do not deduplicate catch handler head blocks. If the
    // handlers are similar, splitting should make this a minimal block of
    // `move-exception` + `goto`.
    if ((g_redex->instrument_mode || kDebugForceInstrumentMode) &&
        cfg.get_pred_edge_of_type(block, cfg::EdgeType::EDGE_THROW)) {
      auto first = block->get_first_insn();
      if (first != block->end() &&
          opcode::is_move_exception(first->insn->opcode())) {
        return false;
      }
    }

    // TODO: It's not worth the goto to merge return-only blocks. What size is
    // the minimum?

    return true;
  }

  static bool begins_with_move_result(cfg::Block* block) {
    const auto first_mie_it = block->get_first_insn();
    if (first_mie_it == block->end()) {
      return false;
    }
    auto first_op = first_mie_it->insn->opcode();
    return opcode::is_move_result_any(first_op);
  }

  bool is_ineligible_because_of_fill_in_stack_trace(cfg::Block* block) {
    if (m_config->dedup_fill_in_stack_trace) {
      return false;
    }
    auto ii = InstructionIterable(block);
    return std::any_of(ii.begin(), ii.end(), [](auto& mie) {
      return dedup_blocks_impl::is_ineligible_because_of_fill_in_stack_trace(
          mie.insn);
    });
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
  // We avoid this situation by skipping blocks that contain an init
  // invocation to an object that didn't come from a unique instruction.
  static boost::optional<std::vector<IRInstruction*>>
  get_init_receiver_instructions_defined_outside_of_block(
      cfg::Block* block, Lazy<LiveRanges>& live_ranges) {
    std::vector<IRInstruction*> res;
    std::unordered_set<IRInstruction*> block_insns;
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (opcode::is_invoke_direct(insn->opcode()) &&
          method::is_init(insn->get_method())) {
        TRACE(DEDUP_BLOCKS, 5, "[dedup blocks] found init invocation: %s",
              SHOW(insn));
        auto& defs = (*live_ranges->use_def_chains)[live_range::Use{insn, 0}];
        always_assert(!defs.empty());
        if (defs.size() > 1) {
          // should never happen, but we are not going to fight that here
          TRACE(DEDUP_BLOCKS, 5, "[dedup blocks] defs.size() = %zu",
                defs.size());
          return boost::none;
        }
        auto def = *defs.begin();
        auto def_opcode = def->opcode();
        always_assert(opcode::is_new_instance(def_opcode) ||
                      opcode::is_a_load_param(def_opcode));
        // Log def instruction if it is not an earlier instruction from the
        // current block.
        if (!block_insns.count(def)) {
          res.push_back(def);
        } else {
          TRACE(DEDUP_BLOCKS, 5, "[dedup blocks] defined in block");
        }
      }
      block_insns.insert(insn);
    }
    return res;
  }

  void check_inits(cfg::ControlFlowGraph& cfg) {
    reaching_defs::MoveAwareFixpointIterator fixpoint_iter(cfg);
    fixpoint_iter.run(reaching_defs::Environment());
    for (cfg::Block* block : cfg.blocks()) {
      auto env = fixpoint_iter.get_entry_state_at(block);
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        if (opcode::is_invoke_direct(insn->opcode()) &&
            method::is_init(insn->get_method())) {
          auto defs = env.get(insn->src(0));
          always_assert(!defs.is_top());
          always_assert(defs.elements().size() == 1);
        }
        fixpoint_iter.analyze_instruction(insn, &env);
      }
    }
  }

  void record_stats(const Duplicates& duplicates) {
    // avoid the expensive lock if we won't actually print the information
    if (traceEnabled(DEDUP_BLOCKS, 2)) {
      for (const auto& entry : duplicates) {
        const auto& blocks = entry.second;
        // all blocks have the same number of opcodes
        cfg::Block* block = *blocks.begin();
        m_stats.dup_sizes[num_opcodes(block)] += blocks.size();
      }
    }
  }

  // remove sets with only one block
  template <typename TKey,
            typename TValue,
            typename THash,
            typename TPred,
            typename NeedToRemove>
  static void remove_if(
      std::unordered_map<TKey, TValue, THash, TPred>& duplicates,
      NeedToRemove need_to_remove) {
    std20::erase_if(duplicates,
                    [&](auto& p) { return need_to_remove(p.second); });
  }

  static bool is_singleton_or_inconsistent(
      const BlockSet& blocks,
      Lazy<LiveRanges>& live_ranges,
      LivenessFixpointIterator& liveness_fixpoint_iter,
      Lazy<type_inference::TypeInference>& type_inference) {
    if (blocks.size() <= 1) {
      return true;
    }

    // Next we check if there are disagreeing init-receiver instructions.
    // TODO: Instead of just dropping all blocks in this case, do
    // finer-grained partitioning.
    boost::optional<std::vector<IRInstruction*>> insns;
    for (cfg::Block* block : blocks) {
      auto other_insns =
          get_init_receiver_instructions_defined_outside_of_block(block,
                                                                  live_ranges);
      if (!other_insns) {
        return true;
      } else if (!insns) {
        insns = std::move(other_insns);
      } else {
        always_assert(insns->size() == other_insns->size());
        for (size_t i = 0; i < insns->size(); i++) {
          if (insns->at(i) != other_insns->at(i)) {
            return true;
          }
        }
      }
    }

    // Next we check if there are inconsistently typed incoming registers.
    // TODO: Instead of just dropping all blocks in this case, do
    // finer-grained partitioning.

    // Initializing stuff...
    const auto& live_in_vars =
        liveness_fixpoint_iter.get_live_in_vars_at(*blocks.begin());
    always_assert(!live_in_vars.is_top());
    if (live_in_vars.is_bottom()) {
      // should never happen, but we are not going to fight that here
      return true;
    }
    // Join together all initial type environments of the the blocks; this
    // corresponds to what will happen when we dedup the blocks.
    auto joined_env = type_inference::TypeEnvironment::bottom();
    for (cfg::Block* block : blocks) {
      joined_env.join_with(type_inference->get_entry_state_at(block));
    }
    if (joined_env.is_bottom()) {
      // shouldn't happen, but we are not going to fight that here
      return true;
    }
    // Let's see if any of the type environments of the existing blocks
    // match, considering the live-in registers. If so, we know that things will
    // verify after deduping.
    for (auto reg : live_in_vars.elements()) {
      auto joined_type = joined_env.get_type(reg);
      if (!type_inference::is_safely_usable_in_ifs(joined_type.element())) {
        return true;
      }
      if (joined_type.element() != IRType::REFERENCE) {
        continue;
      }
      // If any of the reference types we joined might have been of an array
      // type...
      auto joined_type_domain = DexTypeDomain::bottom();
      auto common_dex_type = joined_env.get_dex_type(reg);
      bool maybe_array{false};
      bool maybe_array_of_object{false};
      for (cfg::Block* block : blocks) {
        auto type_domain =
            type_inference->get_entry_state_at(block).get_type_domain(reg);
        auto dex_type = type_domain.get_dex_type();
        if (type_domain.is_top() || !dex_type) {
          maybe_array = true;
          maybe_array_of_object = true;
        } else if (type::is_array(*dex_type)) {
          maybe_array = true;
          if (type::is_object(type::get_array_component_type(*dex_type))) {
            maybe_array_of_object = true;
          }
        }
        joined_type_domain.join_with(type_domain);
        if (common_dex_type && dex_type && *common_dex_type != *dex_type) {
          common_dex_type = boost::none;
        }
      }
      always_assert(!joined_type_domain.is_bottom());
      always_assert(!maybe_array_of_object || maybe_array);
      if (!maybe_array) {
        continue;
      }
      if (common_dex_type) {
        // Trivial join. This is fine.
        continue;
      }
      auto reaching_defs = reaching_defs::Domain::bottom();
      auto init_reaching_defs = [&blocks, &live_ranges, &reaching_defs, reg]() {
        if (reaching_defs.is_bottom()) {
          const auto& fp_iter = live_ranges->chains.get_fp_iter();
          for (cfg::Block* block : blocks) {
            reaching_defs.join_with(fp_iter.get_entry_state_at(block).get(reg));
          }
          always_assert(!reaching_defs.is_bottom());
        }
      };
      if (maybe_array_of_object) {
        // Android's verifier has shortcomings around arrays of interface types,
        // so if there's any use of the register in such a context, we give up.
        // TODO: This is very conservative. Right now, we give up by just
        // looking at the use-instructions. We can be more precise by analyzing
        // the type demand of the instruction, and only give up if it involves
        // an array of an interface type.
        init_reaching_defs();
        for (auto reaching_def : reaching_defs.elements()) {
          for (const auto& use : (*live_ranges->def_use_chains)[reaching_def]) {
            auto op = use.insn->opcode();
            if (opcode::is_an_aget(op) || opcode::is_an_iput(op) ||
                opcode::is_an_sput(op) || opcode::is_an_invoke(op) ||
                opcode::is_a_return(op)) {
              return true;
            }
          }
        }
      }
      // ...but didn't join to an array type...
      if (!joined_type_domain.is_top()) {
        auto joined_dex_type = joined_type_domain.get_dex_type();
        if (joined_dex_type && type::is_array(*joined_dex_type)) {
          continue;
        }
      }
      // ...then we need to make sure that register is not used with an
      // instruction that expects *some* array type.
      init_reaching_defs();
      for (auto reaching_def : reaching_defs.elements()) {
        for (const auto& use : (*live_ranges->def_use_chains)[reaching_def]) {
          auto op = use.insn->opcode();
          if (opcode::is_array_length(op) || opcode::is_an_aget(op) ||
              opcode::is_an_aput(op) || opcode::is_fill_array_data(op)) {
            return true;
          }
        }
      }
    }
    return false;
  }

  static boost::optional<MethodItemEntry&> last_opcode(cfg::Block* block) {
    for (auto it = block->rbegin(); it != block->rend(); it++) {
      if (it->type == MFLOW_OPCODE) {
        return *it;
      }
    }
    return boost::none;
  }

  static size_t num_opcodes(cfg::Block* block) {
    const auto& iterable = InstructionIterable(block);
    return std::distance(iterable.begin(), iterable.end());
  }

  static void print_dups(const Duplicates& dups) {
    TRACE(DEDUP_BLOCKS, 4, "duplicate blocks set: {");
    for (const auto& entry : dups) {
      TRACE(
          DEDUP_BLOCKS, 4, "  hash = %zu",
          DedupBlkValueNumbering::BlockValueHasher{}(*entry.first.block_value));
      for (cfg::Block* b : entry.second) {
        TRACE(DEDUP_BLOCKS, 4, "    block %zu", b->id());
        for (const MethodItemEntry& mie : *b) {
          TRACE(DEDUP_BLOCKS, 4, "      %s", SHOW(mie));
        }
      }
    }
    TRACE(DEDUP_BLOCKS, 4, "} end duplicate blocks set");
  }
};

DedupBlocks::DedupBlocks(const Config* config, DexMethod* method)
    : DedupBlocks(config,
                  method->get_code(),
                  is_static(method),
                  method->get_class(),
                  method->get_proto()->get_args()) {}

DedupBlocks::DedupBlocks(const Config* config,
                         IRCode* code,
                         bool is_static,
                         DexType* declaring_type,
                         DexTypeList* args)
    : m_config(config),
      m_code(code),
      m_is_static(is_static),
      m_declaring_type(declaring_type),
      m_args(args) {
  always_assert(m_config);
}

void DedupBlocks::run() {
  DedupBlocksImpl impl(m_config, m_stats);
  auto& cfg = m_code->cfg();
  uint32_t iteration = 0;
  do {
    ++iteration;
    if (m_config->split_postfix) {
      impl.split_postfix(cfg);
    }
  } while (impl.dedup(m_is_static, m_declaring_type, m_args, cfg) &&
           iteration < m_config->max_iteration);
}

Stats& Stats::operator+=(const Stats& that) {
  eligible_blocks += that.eligible_blocks;
  blocks_removed += that.blocks_removed;
  insns_removed += that.insns_removed;
  blocks_split += that.blocks_split;
  positions_inserted += that.positions_inserted;
  for (auto& p : that.dup_sizes) {
    dup_sizes[p.first] += p.second;
  }
  return *this;
}

} // namespace dedup_blocks_impl
