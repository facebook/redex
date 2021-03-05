/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_set>

#include "ControlFlow.h"
#include "CppUtil.h"
#include "Debug.h"
#include "IRList.h"

class DexMethod;

namespace source_blocks {

using namespace cfg;

namespace impl {

struct BlockAccessor {
  static void push_source_block(Block* b,
                                std::unique_ptr<SourceBlock> src_block) {
    auto it = b->get_first_non_param_loading_insn();
    if (it != b->end() &&
        (opcode::is_a_move_result_pseudo(it->insn->opcode()) ||
         opcode::is_a_move_result(it->insn->opcode()))) {
      ++it;
    }
    auto mie = new MethodItemEntry(std::move(src_block));
    if (it == b->end()) {
      b->m_entries.push_back(*mie);
    } else {
      b->m_entries.insert_before(it, *mie);
    }
  }
};

template <typename BlockStartFn, typename EdgeFn, typename BlockEndFn>
void visit_in_order(const ControlFlowGraph* cfg,
                    const BlockStartFn& block_start_fn,
                    const EdgeFn& edge_fn,
                    const BlockEndFn& block_end_fn) {
  // Do not rely on `blocks()`, as there are no ordering guarantees. For now,
  // do a simple DFS with explicitly ordered edges.

  auto get_sorted_edges = [](Block* b) {
    auto succs = b->succs();
    std::sort(succs.begin(), succs.end(), [](const Edge* lhs, const Edge* rhs) {
      if (lhs->type() != rhs->type()) {
        return lhs->type() < rhs->type();
      }
      switch (lhs->type()) {
      case EDGE_GOTO:
        redex_assert(lhs == rhs);
        return false;
      case EDGE_BRANCH: {
        auto lhs_case = lhs->case_key();
        auto rhs_case = rhs->case_key();
        if (!lhs_case) {
          redex_assert(!rhs_case);
          redex_assert(lhs == rhs);
          return false;
        }
        redex_assert(rhs_case);
        return *lhs_case < *rhs_case;
      }
      case EDGE_THROW: {
        auto lhs_info = lhs->throw_info();
        auto rhs_info = rhs->throw_info();
        redex_assert(lhs_info != nullptr);
        redex_assert(rhs_info != nullptr);
        auto lhs_catch = lhs_info->catch_type;
        auto rhs_catch = rhs_info->catch_type;
        if (lhs_catch == nullptr) {
          if (rhs_catch == nullptr) {
            redex_assert(lhs == rhs);
            return false;
          }
          return true;
        }
        if (rhs_catch == nullptr) {
          return false;
        }
        return compare_dextypes(lhs_catch, rhs_catch);
      }
      case EDGE_GHOST:
        return false;
      case EDGE_TYPE_SIZE:
        not_reached();
      }
      not_reached(); // For GCC.
    });
    return succs;
  };

  std::unordered_set<Block*> visited;
  self_recursive_fn(
      [&](auto self, Block* cur) {
        if (visited.count(cur)) {
          return;
        }
        visited.insert(cur);

        block_start_fn(cur);

        for (const auto* e : get_sorted_edges(cur)) {
          if (e->type() == EDGE_GHOST) {
            continue;
          }
          edge_fn(cur, e);
          self(self, e->target());
        }

        block_end_fn(cur);
      },
      cfg->entry_block());

  redex_assert(visited.size() == cfg->blocks().size());
}

} // namespace impl

struct InsertResult {
  size_t block_count;
  std::string serialized;
};

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  bool serialize = true);

inline bool has_source_blocks(const cfg::Block* b) {
  for (const auto& mie : *b) {
    if (mie.type == MFLOW_SOURCE_BLOCK) {
      return true;
    }
  }
  return false;
}

inline std::vector<const SourceBlock*> gather_source_blocks(
    const cfg::Block* b) {
  std::vector<const SourceBlock*> ret;
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    ret.push_back(mie.src_block.get());
  }
  return ret;
}

} // namespace source_blocks
