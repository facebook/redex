/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
#include "Resolver.h"
#include "Transform.h"
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
 * arguably, having stack traces that point to similar looking code (in a different
 * location) is better than having stack traces point to the nearest line of
 * source code before or after the merged block.
 *
 * Deleting the line info would also make things complicated if `cleanup()` is
 * inlined into `getBar()`. We would be unable to reconstruct the inlined stack
 * frame if we deleted the callsite's line number.
 */

namespace {

using hash_t = std::size_t;

struct BlockEquals {
  bool operator()(cfg::Block* b1, cfg::Block* b2) const {
    return same_successors(b1, b2) && b1->same_try(b2) &&
           same_code(b1, b2);
  }

  // Structural equality of opcodes
  static bool same_code(cfg::Block* b1, cfg::Block* b2) {
    return InstructionIterable(b1).structural_equals(
        InstructionIterable(b2));
  }

  // The blocks must also have the exact same successors
  // (and reached in the same ways)
  static bool same_successors(cfg::Block* b1, cfg::Block* b2) {
    const auto& b1_succs = b1->succs();
    const auto& b2_succs = b2->succs();
    if (b1_succs.size() != b2_succs.size()) {
      return false;
    }
    for (const cfg::Edge* b1_succ : b1_succs) {
      const auto& in_b2 =
          std::find_if(b2_succs.begin(),
                       b2_succs.end(),
                       [&](const cfg::Edge* e) {
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
    return *a < *b;
  }
};

class DedupBlocksImpl {
 public:
  DedupBlocksImpl(const std::vector<DexClass*>& scope,
                  PassManager& mgr,
                  const DedupBlocksPass::Config& config)
      : m_scope(scope), m_mgr(mgr), m_config(config) {}

  void run() {
    walk::parallel::code(m_scope, [this](DexMethod* method, IRCode& code) {
      if (m_config.method_black_list.count(method) != 0) {
        return;
      }

      code.build_cfg(true);
      auto& cfg = code.cfg();

      Duplicates dups = collect_duplicates(cfg);
      if (dups.size() > 0) {
        record_stats(dups);
        deduplicate(dups, cfg);
      }

      code.clear_cfg();
    });
    report_stats();
  }

 private:
  using Duplicates = std::unordered_map<cfg::Block*,
                                        std::set<cfg::Block*, BlockCompare>,
                                        BlockHasher,
                                        BlockEquals>;
  const char* METRIC_BLOCKS_REMOVED = "blocks_removed";
  const char* METRIC_ELIGIBLE_BLOCKS = "eligible_blocks";
  const std::vector<DexClass*>& m_scope;
  PassManager& m_mgr;
  const DedupBlocksPass::Config& m_config;

  std::atomic_int m_num_eligible_blocks{0};
  std::atomic_int m_num_blocks_removed{0};

  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> m_dup_sizes;
  std::mutex lock;

  // Find blocks with the same exact code
  Duplicates collect_duplicates(const cfg::ControlFlowGraph& cfg) {
    const auto& blocks = cfg.blocks();
    Duplicates duplicates;

    for (cfg::Block* block : blocks) {
      if (is_eligible(block)) {
        duplicates[block].insert(block);
        ++m_num_eligible_blocks;
      }
    }

    remove_singletons(duplicates);
    return duplicates;
  }

  // remove all but one of a duplicate set. Reroute the predecessors to the
  // canonical block
  void deduplicate(const Duplicates& dups, cfg::ControlFlowGraph& cfg) {

    fix_dex_pos_pointers(dups, cfg);

    for (const auto& entry : dups) {
      const auto& blocks = entry.second;

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
  }

  // DexPositions have `parent` pointers to other DexPositions inside the same
  // method. We will delete some of these DexPositions, which would create
  // dangling pointers.
  //
  // This method changes those parent pointers to the equivalent DexPosition
  // in the canonical block
  void fix_dex_pos_pointers(const Duplicates& dups,
                            cfg::ControlFlowGraph& cfg) {
    // A map from the DexPositions we're about to delete to the equivalent
    // DexPosition in the canonical block.
    std::unordered_map<DexPosition*, DexPosition*> position_replace_map;

    for (const auto& entry : dups) {
      const auto& blocks = entry.second;

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

    if (block->is_catch()) {
      // TODO. Should be possible. Skip now for simplicity
      return false;
    }

    // We can't split up move-result(-pseudo) instruction pairs
    if (begins_with_move_result(block)) {
      return false;
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
    // We try to avoid this situation by not considering blocks that call
    // constructors, but this isn't bulletproof. FIXME
    if (calls_constructor(block)) {
      // TODO(T27582908): This could be more precise
      return false;
    }

    return true;
  }

  static bool begins_with_move_result(cfg::Block* block) {
    const auto& first_mie = *block->get_first_insn();
    auto first_op = first_mie.insn->opcode();
    return is_move_result(first_op) || opcode::is_move_result_pseudo(first_op);
  }

  static bool calls_constructor(cfg::Block* block) {
    for (const auto& mie : InstructionIterable(block)) {
      if (is_invoke_direct(mie.insn->opcode())) {
        auto meth =
            resolve_method(mie.insn->get_method(), MethodSearch::Direct);
        if (meth != nullptr && is_init(meth)) {
          return true;
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
    m_mgr.incr_metric(METRIC_ELIGIBLE_BLOCKS, eligible_blocks);
    m_mgr.incr_metric(METRIC_BLOCKS_REMOVED, removed);
    TRACE(DEDUP_BLOCKS, 2, "%d eligible_blocks\n", eligible_blocks);

    for (const auto& entry : m_dup_sizes) {
      TRACE(DEDUP_BLOCKS,
            2,
            "found %d duplicate blocks with %d instructions\n",
            entry.second,
            entry.first);
    }

    TRACE(DEDUP_BLOCKS, 1, "%d blocks removed\n", removed);
  }

  // remove sets with only one block
  static void remove_singletons(Duplicates& duplicates) {
    for (auto it = duplicates.begin(); it != duplicates.end();) {
      if (it->second.size() <= 1) {
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
}

void DedupBlocksPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  DedupBlocksImpl impl(scope, mgr, m_config);
  impl.run();
}

static DedupBlocksPass s_pass;
