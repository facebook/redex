/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DedupBlocksPass.h"

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Transform.h"
#include "Walkers.h"

#include <boost/optional.hpp>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

/*
 * This pass removes blocks that are duplicates in a method.
 *
 * If a method has multiple blocks with the same code and the same successors,
 * delete all but one of the blocks. Naming one of them the canonical block.
 *
 * Then, reroute all the predecessors of all the blocks to that canonical block.
 */

namespace {

class DedupBlocksImpl {
 public:
  DedupBlocksImpl(const std::vector<DexClass*>& scope,
                  PassManager& mgr,
                  const DedupBlocksPass::Config& config)
      : m_scope(scope), m_mgr(mgr), m_config(config) {}

  void run() {
    walk_methods(m_scope, [this](DexMethod* method) {
      if (m_config.method_black_list.count(method) != 0) {
        return;
      }

      IRCode* code = method->get_code();
      if (code == nullptr) {
        return;
      }
      code->build_cfg();

      duplicates_t dups = collect_duplicates(code);
      if (dups.size() > 0) {
        record_stats(dups);
        deduplicate(dups, method);
      }
    });
    report_stats();
  }

 private:
  using hash_t = uint64_t;
  using duplicates_t = std::unordered_map<hash_t, std::unordered_set<Block*>>;
  const char* METRIC_BLOCKS_REMOVED = "blocks_removed";
  const std::vector<DexClass*>& m_scope;
  PassManager& m_mgr;
  const DedupBlocksPass::Config& m_config;

  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> m_dup_sizes;

  // Find blocks with the same exact code
  duplicates_t collect_duplicates(IRCode* code) {
    const auto& blocks = code->cfg().blocks();
    std::unordered_map<hash_t, std::unordered_set<Block*>> duplicates;
    for (Block* block : blocks) {
      if (has_opcodes(block) && should_remove(code->cfg(), block)) {
        duplicates[hash(block)].insert(block);
      }
    }

    remove_singletons(duplicates);
    // the hash function isn't perfect, so check behind with an equals function
    remove_invalid_sets(code, duplicates);
    return duplicates;
  }

  // remove all but one of a duplicate set. Reroute the predecessors to the
  // canonical block
  void deduplicate(const duplicates_t& dups, DexMethod* method) {
    const auto& code = method->get_code();
    for (const auto& entry : dups) {
      std::unordered_set<Block*> blocks = entry.second;

      // canon is block with lowest id.
      Block* canon = nullptr;
      size_t canon_id = std::numeric_limits<size_t>::max();
      for (Block* block : blocks) {
        size_t id = block->id();
        if (id <= canon_id) {
          canon_id = id;
          canon = block;
        }
      }

      for (Block* block : blocks) {
        if (block->id() == canon_id) {
          // We remove the debug line information because it will be incorrect
          // for every block we reroute here. When there's no debug info,
          // the jvm will report the error on the closing brace of the function.
          // It's not perfect but it's better than incorrect information.
          code->remove_debug_line_info(canon);
        } else {
          transform::replace_block(code, block, canon);
          m_mgr.incr_metric(METRIC_BLOCKS_REMOVED, 1);
        }
      }
    }
  }

  void remove_invalid_sets(IRCode* code, duplicates_t& dups) {
    for (auto it = dups.begin(); it != dups.end();) {
      std::unordered_set<Block*> blocks = it->second;
      if (!all_equal(blocks) || conflicting_try_regions(code, blocks)) {
        it = dups.erase(it);
      } else {
        ++it;
      }
    }
  }

  static bool should_remove(const ControlFlowGraph& cfg, Block* block) {
    if (!has_opcodes(block)) {
      return false;
    }

    if (is_catch(block)) {
      // TODO. Should be possible. Skip now for simplicity
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
      return false;
    }

    for (Block* pred : block->preds()) {
      if (!has_opcodes(pred)) {
        // Skip these for simplicity's sake. I don't want to go recursively
        // searching for the previous block with code. But you could! TODO
        return false;
      }

      if (is_fallthrough(cfg, pred, block)) {
        return false;
      }
    }

    for (Block* succ : block->succs()) {
      if (is_fallthrough(cfg, block, succ)) {
        return false;
      }
    }

    return true;
  }

  static bool calls_constructor(Block* block) {
    for (const auto& mie : InstructionIterable(block)) {
      if (is_invoke(mie.insn->opcode()) && is_init(mie.insn->get_method())) {
        return true;
      }
    }
    return false;
  }

  // `pred` falls through to `succ` iff their connecting edge in the CFG is a
  // goto but `pred`'s last instruction isn't a goto
  static bool is_fallthrough(const ControlFlowGraph& cfg,
                             Block* pred,
                             Block* succ) {
    always_assert_log(has_opcodes(pred), "need opcodes");

    const auto& flags = cfg.edge(pred, succ);
    if (!(flags[EDGE_GOTO])) {
      return false;
    }

    const auto& last_of_pred = last_opcode(pred);
    if (!is_goto(last_of_pred->insn->opcode())) {
      return true;
    }
    return false;
  }

  void record_stats(const duplicates_t& duplicates) {
    for (const auto& entry : duplicates) {
      bool first = true;
      bool same_size = true;
      size_t size = 0;
      for (Block* block : entry.second) {
        size_t this_size = num_opcodes(block);
        if (first) {
          size = this_size;
          first = false;
        } else if (size != this_size) {
          same_size = false;
        }
      }
      if (same_size && size > 0) {
        m_dup_sizes[size] += entry.second.size();
      }
    }
  }

  void report_stats() {
    for (const auto& entry : m_dup_sizes) {
      TRACE(DEDUP_BLOCKS,
            2,
            "found %d duplicate blocks with %d instructions\n",
            entry.second,
            entry.first);
    }

    TRACE(DEDUP_BLOCKS,
          1,
          "%d blocks removed\n",
          m_mgr.get_metric(METRIC_BLOCKS_REMOVED));
  }

  // remove sets with only one block
  static void remove_singletons(duplicates_t& duplicates) {
    for (auto it = duplicates.begin(); it != duplicates.end();) {
      if (it->second.size() <= 1) {
        it = duplicates.erase(it);
      } else {
        ++it;
      }
    }
  }

  static boost::optional<MethodItemEntry&> last_opcode(Block* block) {
    for (auto it = block->rbegin(); it != block->rend(); it++) {
      if (it->type == MFLOW_OPCODE) {
        return *it;
      }
    }
    return boost::none;
  }

  static bool has_opcodes(Block* block) {
    const auto& it = InstructionIterable(block);
    return it.begin() != it.end();
  }

  static size_t num_opcodes(Block* block) {
    size_t result = 0;
    const auto& iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      result++;
    }
    return result;
  }

  // return true if all the blocks don't jump to the same catch handler (or no
  // catch handler at all)
  static bool conflicting_try_regions(IRCode* code,
                                      std::unordered_set<Block*> blocks) {
    MethodItemEntry* active_catch = nullptr;
    bool first = true;
    for (Block* b : blocks) {
      if (first) {
        first = false;
        active_catch = transform::find_active_catch(code, b->begin());
      } else {
        if (active_catch != transform::find_active_catch(code, b->begin())) {
          return true;
        }
      }
    }
    return false;
  }

  static bool all_equal(std::unordered_set<Block*> blocks) {
    Block* canon = nullptr;
    for (auto i = blocks.begin(); i != blocks.end(); i++) {
      if (canon == nullptr) {
        canon = *i;
      } else if (!equals(canon, *i)) {
        return false;
      }
    }
    return true;
  }

  // Structural equality of opcodes except branches targets are ignored
  // because they are unknown until we sync back to DexInstructions.
  //
  // The blocks must also have the exact same successors
  static bool equals(Block* b1, Block* b2) {

    if (!same_successors(b1, b2)) {
      return false;
    }

    const auto& iterable1 = InstructionIterable(b1);
    const auto& iterable2 = InstructionIterable(b2);
    auto it1 = iterable1.begin();
    auto it2 = iterable2.begin();
    for (; it1 != iterable1.end() && it2 != iterable2.end(); it1++, it2++) {
      auto& mie1 = *it1;
      auto& mie2 = *it2;
      if (*mie1.insn != *mie2.insn) {
        return false;
      }
    }

    if (!(it1 == iterable1.end() && it2 == iterable2.end())) {
      // different lengths
      return false;
    }
    return true;
  }

  static bool same_successors(Block* b1, Block* b2) {

    const auto& b1_succs = b1->succs();
    const auto& b2_succs = b2->succs();
    if (b1_succs.size() != b2_succs.size()) {
      return false;
    }
    for (Block* b1_succ : b1_succs) {
      if (std::find(b2_succs.begin(), b2_succs.end(), b1_succ) ==
          b2_succs.end()) {
        // b1 has a succ that b2 doesn't
        return false;
      }
    }
    return true;
  }

  static hash_t hash(Block* block) {
    hash_t result = 0;
    for (auto& mie : InstructionIterable(block)) {
      result ^= hash(mie.insn);
    }
    return result;
  }

  static hash_t hash(IRInstruction* insn) {
    std::vector<hash_t> bits;
    bits.push_back(insn->opcode());
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      bits.push_back(insn->src(i));
    }
    if (insn->dests_size() > 0) {
      bits.push_back(insn->dest());
    }
    if (insn->has_data()) {
      size_t size = insn->get_data()->size();
      const auto& data = insn->get_data()->data();
      for (size_t i = 0; i < size; i++) {
        bits.push_back(data[i]);
      }
    }
    if (insn->has_type()) {
      bits.push_back(reinterpret_cast<hash_t>(insn->get_type()));
    }
    if (insn->has_field()) {
      bits.push_back(reinterpret_cast<hash_t>(insn->get_field()));
    }
    if (insn->has_method()) {
      bits.push_back(reinterpret_cast<hash_t>(insn->get_method()));
    }
    if (insn->has_string()) {
      bits.push_back(reinterpret_cast<hash_t>(insn->get_string()));
    }
    if (opcode::has_range(insn->opcode())) {
      bits.push_back(insn->range_base());
      bits.push_back(insn->range_size());
    }
    bits.push_back(insn->literal());
    // ignore insn->offset because its not known until sync to DexInstructions

    hash_t result = 0;
    for (hash_t elem : bits) {
      result ^= elem;
    }
    return result;
  }

  static void print_dups(duplicates_t dups) {
    TRACE(DEDUP_BLOCKS, 4, "duplicate blocks set: {\n");
    for (const auto& entry : dups) {
      TRACE(DEDUP_BLOCKS, 4, "  hash = %lu\n", entry.first);
      for (Block* b : entry.second) {
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
