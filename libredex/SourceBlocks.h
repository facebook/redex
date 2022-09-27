/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "ControlFlow.h"
#include "CppUtil.h"
#include "Debug.h"
#include "IRCode.h"
#include "IRList.h"

class DexMethod;
class DexStore;
class ScopedMetrics;

// Must match DexStore.
using DexStoresVector = std::vector<DexStore>;

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

  static IRList::iterator insert_source_block_after(
      Block* b,
      const IRList::iterator& it,
      std::unique_ptr<SourceBlock> src_block) {
    auto mie = new MethodItemEntry(std::move(src_block));
    return b->m_entries.insert_after(it, *mie);
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
  bool profile_success;
};

// Source data for a profile = interaction. Three options per interactions:
// * Nothing (std::nullopt)
// * A string that denotes a serialized profile, and an error value, in case
//   the profile does nto match the CFG.
// * A general default value.
using ProfileData =
    std::variant<std::nullopt_t,
                 std::pair<std::string, boost::optional<SourceBlock::Val>>,
                 SourceBlock::Val>;

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles = {},
                                  bool serialize = true,
                                  bool insert_after_excs = false);

bool has_source_block_positive_val(const SourceBlock* sb);

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
    for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
      ret.push_back(sb);
    }
  }
  return ret;
}

inline std::vector<SourceBlock*> gather_source_blocks(cfg::Block* b) {
  std::vector<SourceBlock*> ret;
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
      ret.push_back(sb);
    }
  }
  return ret;
}

template <typename Fn>
inline void foreach_source_block(cfg::Block* b, const Fn& fn) {
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
      fn(sb);
    }
  }
}
template <typename Fn>
inline void foreach_source_block(const cfg::Block* b, const Fn& fn) {
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
      fn(sb);
    }
  }
}

inline SourceBlock* get_first_source_block(cfg::Block* b) {
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    return mie.src_block.get();
  }
  return nullptr;
}
inline const SourceBlock* get_first_source_block(const cfg::Block* b) {
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    return mie.src_block.get();
  }
  return nullptr;
}

inline SourceBlock* get_last_source_block_before(cfg::Block* b,
                                                 IRList::iterator it) {
  while (it != b->begin()) {
    it--;
    if (it->type == MFLOW_SOURCE_BLOCK) {
      return it->src_block->get_last_in_chain();
    }
  }
  return nullptr;
}
inline const SourceBlock* get_last_source_block_before(
    const cfg::Block* b, IRList::const_iterator it) {
  while (it != b->begin()) {
    it--;
    if (it->type == MFLOW_SOURCE_BLOCK) {
      return it->src_block->get_last_in_chain();
    }
  }
  return nullptr;
}

inline SourceBlock* get_first_source_block(cfg::ControlFlowGraph* cfg) {
  for (auto* b : cfg->blocks()) {
    auto sb = get_first_source_block(b);
    if (sb != nullptr) {
      return sb;
    }
  }
  return nullptr;
}

inline SourceBlock* get_first_source_block(IRCode* code) {
  if (code->cfg_built()) {
    return get_first_source_block(&code->cfg());
  } else {
    for (const auto& mie : *code) {
      if (mie.type != MFLOW_SOURCE_BLOCK) {
        continue;
      }
      return mie.src_block.get();
    }
    return nullptr;
  }
}

inline SourceBlock* get_last_source_block(cfg::Block* b) {
  auto rit = std::find_if(b->rbegin(), b->rend(), [](const auto& mie) {
    return mie.type == MFLOW_SOURCE_BLOCK;
  });

  if (rit == b->rend()) {
    return nullptr;
  }

  return rit->src_block.get();
}
inline const SourceBlock* get_last_source_block(const cfg::Block* b) {
  return const_cast<SourceBlock*>(
      get_last_source_block(const_cast<cfg::Block*>(b)));
}

IRList::iterator find_first_block_insert_point(cfg::Block* b);

void track_source_block_coverage(ScopedMetrics& sm,
                                 const DexStoresVector& stores);

class SourceBlockConsistencyCheck;
SourceBlockConsistencyCheck& get_sbcc();

struct ViolationsHelper {
  struct ViolationsHelperImpl;
  std::unique_ptr<ViolationsHelperImpl> impl;

  enum class Violation {
    kHotImmediateDomNotHot,
    kChainAndDom,
  };

  ViolationsHelper(Violation v,
                   const Scope& scope,
                   std::vector<std::string> to_vis);
  ~ViolationsHelper();
};

} // namespace source_blocks
