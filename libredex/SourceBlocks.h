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

inline std::vector<Edge*> get_sorted_edges(Block* b) {
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
}

template <typename BlockStartFn, typename EdgeFn, typename BlockEndFn>
void visit_in_order(const ControlFlowGraph* cfg,
                    const BlockStartFn& block_start_fn,
                    const EdgeFn& edge_fn,
                    const BlockEndFn& block_end_fn) {
  // Do not rely on `blocks()`, as there are no ordering guarantees. For now,
  // do a simple DFS with explicitly ordered edges.

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

  redex_assert(visited.size() == cfg->num_blocks());
}

} // namespace impl

struct InsertResult {
  size_t block_count;
  std::string serialized;
  std::string serialized_idom_map;
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

InsertResult insert_source_blocks(const DexString* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles = {},
                                  bool serialize = true,
                                  bool insert_after_excs = false);

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

template <typename Iterator>
inline SourceBlock* find_between(const Iterator& start, const Iterator& end) {
  auto it = std::find_if(
      start, end, [](auto& e) { return e.type == MFLOW_SOURCE_BLOCK; });
  return it != end ? it->src_block.get() : nullptr;
}

inline SourceBlock* get_last_source_block_before(cfg::Block* b,
                                                 const IRList::iterator& it) {
  auto* sb = find_between(IRList::reverse_iterator(it), b->rend());
  return sb != nullptr ? sb->get_last_in_chain() : nullptr;
}
inline const SourceBlock* get_last_source_block_before(
    const cfg::Block* b, IRList::const_iterator it) {
  auto* sb = find_between(IRList::const_reverse_iterator(it), b->rend());
  return sb != nullptr ? sb->get_last_in_chain() : nullptr;
}

inline SourceBlock* get_first_source_block_after(cfg::Block* b,
                                                 const IRList::iterator& it) {
  auto* sb = find_between(it, b->end());
  return sb != nullptr ? sb : nullptr;
}
inline const SourceBlock* get_first_source_block_after(
    const cfg::Block* b, IRList::const_iterator it) {
  auto* sb = find_between(it, b->end());
  return sb != nullptr ? sb : nullptr;
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

namespace normalize {

size_t num_interactions(const cfg::ControlFlowGraph& cfg,
                        const SourceBlock* sb);

inline float get_factor(SourceBlock* dominating,
                        SourceBlock* dominated,
                        size_t idx) {
  float caller_val;
  {
    if (dominating == nullptr) {
      return NAN;
    }
    auto val = dominating->get_val(idx);
    if (!val) {
      return NAN;
    }
    caller_val = *val;
  }
  if (caller_val == 0) {
    return 0.0f;
  }

  float callee_val;
  {
    if (dominated == nullptr) {
      return NAN;
    }
    auto val = dominated->get_val(idx);
    if (!val) {
      return NAN;
    }
    callee_val = *val;
  }
  if (callee_val == 0) {
    return 0.0f;
  }

  // Expectation would be that callee_val >= caller_val. But tracking might
  // not be complete.

  // This will normalize to the value at the dominating source block.
  return caller_val / callee_val;
}

inline void normalize(SourceBlock* sb, size_t idx, float factor) {
  if (sb->vals[idx]) {
    sb->vals[idx]->val *= factor;
  }
}

inline void normalize(SourceBlock* dominating,
                      SourceBlock* dominated,
                      size_t interactions) {
  for (size_t i = 0; i != interactions; ++i) {
    auto sb_factor = get_factor(dominating, dominated, i);
    normalize(dominated, i, sb_factor);
  }
}

inline void normalize(ControlFlowGraph& cfg,
                      SourceBlock* dominating,
                      SourceBlock* dominated,
                      size_t interactions) {
  if (interactions == 0) {
    return;
  }
  std::vector<float> factors;
  factors.reserve(interactions);
  for (size_t i = 0; i != interactions; ++i) {
    factors.push_back(get_factor(dominating, dominated, i));
  }
  for (auto* b : cfg.blocks()) {
    source_blocks::foreach_source_block(b, [&](auto* sb) {
      for (size_t i = 0; i != interactions; ++i) {
        normalize(sb, i, factors[i]);
      }
    });
  }
}

inline void normalize(ControlFlowGraph& cfg,
                      SourceBlock* dominating,
                      size_t interactions) {
  // Assume that integrity is guaranteed, so that val at entry is
  // dominating all blocks.
  normalize(
      cfg, dominating, get_first_source_block(cfg.entry_block()), interactions);
}

} // namespace normalize

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
                   size_t top_n,
                   std::vector<std::string> to_vis);
  ~ViolationsHelper();

  void process(ScopedMetrics* sm);
  void silence();

  ViolationsHelper(ViolationsHelper&& other) noexcept;
  ViolationsHelper& operator=(ViolationsHelper&& rhs) noexcept;
};

SourceBlock* get_first_source_block_of_method(const DexMethod* m);

SourceBlock* get_any_first_source_block_of_methods(
    const std::vector<const DexMethod*>& methods);

void insert_synthetic_source_blocks_in_method(
    DexMethod* method,
    const std::function<std::unique_ptr<SourceBlock>()>& source_block_creator);

void fill_source_block(SourceBlock& sb,
                       DexMethod* ref,
                       uint32_t id,
                       const SourceBlock::Val& val);

void fill_source_block(
    SourceBlock& sb,
    DexMethod* ref,
    uint32_t id,
    const std::optional<SourceBlock::Val>& opt_val = std::nullopt);
    
} // namespace source_blocks
