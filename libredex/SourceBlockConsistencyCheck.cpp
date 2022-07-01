/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlockConsistencyCheck.h"

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "IRCode.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace source_blocks {

bool operator<(const SourceBlockInfo& l, const SourceBlockInfo& r) {
  if (l.original_dex_method != r.original_dex_method) {
    return compare_dexmethods(l.original_dex_method, r.original_dex_method);
  }

  return l.id < r.id;
}

bool operator==(const SourceBlockInfo& l, const SourceBlockInfo& r) {
  return std::tie(l.original_dex_method, l.id) ==
         std::tie(r.original_dex_method, r.id);
}

SourceBlockDomInfo::SourceBlockDomInfo(const cfg::ControlFlowGraph& cfg,
                                       uint32_t num_src_blks)
    : m_dom_tree(cfg, num_src_blks) {}

std::vector<SourceBlockInfo> SourceBlockDomInfo::get_removeable_src_blks() {
  return std::vector<SourceBlockInfo>{m_dom_tree.leaves().begin(),
                                      m_dom_tree.leaves().end()};
}

void SourceBlockDomInfo::remove_src_blk(const SourceBlockInfo& sb_info) {
  m_dom_tree.remove_src_blk(sb_info);
}

void SourceBlockConsistencyCheck::initialize(const Scope& scope) {
  always_assert(!this->is_initialized());
  this->m_is_initialized = true;

  // Preallocate slots in the map, so we can get
  // at them when multithreaded without needing synchronization
  walk::methods(scope, [this](DexMethod* dex_method) {
    this->m_context_map.insert({dex_method, SBConsistencyContext{}});
  });

  walk::parallel::methods(scope, [this](DexMethod* dex_method) {
    auto it = this->m_context_map.find(dex_method);
    if (it == this->m_context_map.end()) {
      return;
    }

    IRCode* code = dex_method->get_code();

    if (code == nullptr) {
      return;
    }

    cfg::ScopedCFG scfg(code);
    cfg::ControlFlowGraph& cfg = code->cfg();
    cfg.calculate_exit_block();

    this->rebuild_sbdi(dex_method, cfg);
  });
}

void SourceBlockConsistencyCheck::rebuild_sbdi(DexMethod* dex_method,
                                               const ControlFlowGraph& cfg) {

  auto it = this->m_context_map.find(dex_method);
  if (it == this->m_context_map.end()) {
    return;
  }

  auto& sbcc = it->second;
  sbcc.m_source_blocks.clear();
  for (auto* b : cfg.blocks()) {
    source_blocks::foreach_source_block(b, [&sbcc](auto& sb) {
      sbcc.m_source_blocks.insert(SourceBlockInfo{sb->src, sb->id});
    });
  }

  sbcc.m_sbdi = SourceBlockDomInfo{
      cfg, static_cast<uint32_t>(sbcc.m_source_blocks.size())};
}

template <class T>
struct merge_vecs {
  void operator()(const T& addend, T* accumulator) const {
    accumulator->insert(accumulator->end(), addend.begin(), addend.end());
  }
};

size_t SourceBlockConsistencyCheck::run(const Scope& scope) {
  struct Failure {
    DexMethod* dex_method = nullptr;
    std::vector<SourceBlockInfo> src_blks;
  };

  auto res = walk::parallel::methods<std::vector<Failure>,
                                     merge_vecs<std::vector<Failure>>>(
      scope, [this](DexMethod* dex_method) -> std::vector<Failure> {
        auto it = this->m_context_map.find(dex_method);
        if (it == this->m_context_map.end()) {
          return {};
        }

        auto& sbcc = it->second;
        auto& [source_blocks, known_missing_source_blocks, sbdi] = sbcc;

        IRCode* code = dex_method->get_code();

        if (code == nullptr) {
          return {};
        }

        cfg::ScopedCFG scfg(code);
        const cfg::ControlFlowGraph& cfg = code->cfg();

        std::set<SourceBlockInfo> source_blocks_in_ir;
        for (cfg::Block* block : cfg.blocks()) {
          source_blocks::foreach_source_block(
              block, [&source_blocks_in_ir](auto& sb) {
                source_blocks_in_ir.insert({sb->src, sb->id});
              });
        }

        std::vector<SourceBlockInfo> missing;
        std::set_difference(source_blocks.begin(), source_blocks.end(),
                            source_blocks_in_ir.begin(),
                            source_blocks_in_ir.end(),
                            std::back_inserter(missing));

        missing.erase(std::remove_if(
                          missing.begin(), missing.end(),
                          [&sbcc](const auto& s) {
                            return sbcc.m_known_missing_source_blocks.find(s) !=
                                   sbcc.m_known_missing_source_blocks.end();
                          }),
                      missing.end());

        if (missing.empty()) {
          return {};
        }
        known_missing_source_blocks.insert(missing.begin(), missing.end());

        std::sort(missing.begin(), missing.end());

        for (bool changed = true; changed;) {
          changed = false;

          auto removeable_blks = sbdi.get_removeable_src_blks();
          std::sort(removeable_blks.begin(), removeable_blks.end());

          std::vector<SourceBlockInfo> blks_to_remove;
          std::set_intersection(removeable_blks.begin(), removeable_blks.end(),
                                missing.begin(), missing.end(),
                                std::back_inserter(blks_to_remove));

          if (!blks_to_remove.empty()) {
            changed = true;

            missing.erase(std::remove_if(missing.begin(), missing.end(),
                                         [&blks_to_remove](auto& m) {
                                           auto it_blks_to_remove =
                                               std::lower_bound(
                                                   blks_to_remove.begin(),
                                                   blks_to_remove.end(), m);
                                           return it_blks_to_remove !=
                                                      blks_to_remove.end() &&
                                                  *it_blks_to_remove == m;
                                         }),
                          missing.end());

            for (const auto& sb_info : blks_to_remove) {
              sbdi.remove_src_blk(sb_info);
            }
          }
        }

        if (missing.empty()) {
          return {};
        }

        return std::vector<Failure>{{dex_method, std::move(missing)}};
      });

  if (!res.empty()) {
    int num_missing_blks = std::accumulate(
        res.begin(), res.end(), 0,
        [](const auto& l, const auto& r) { return l + r.src_blks.size(); });

    TRACE(SBCC, 2,
          "Pass introduced  %d missing source blocks across %zu methods.",
          num_missing_blks, res.size());

    for (const auto& [dex_method, src_blks] : res) {

      always_assert(!src_blks.empty());

      std::map<DexMethodRef*, std::vector<uint32_t>, dexmethods_comparator>
          methodRefToSrcBlockIds;
      for (const auto& [src, id] : src_blks) {
        methodRefToSrcBlockIds[src].push_back(id);
      }

      TRACE(SBCC, 2, "  Missing source blocks in method, %s",
            show_deobfuscated(dex_method).c_str());

      for (const auto& [src, ids] : methodRefToSrcBlockIds) {
        auto meth_name = show_deobfuscated(src);
        std::string idListStr = std::accumulate(
            ids.begin() + 1, ids.end(), std::to_string(ids.front()),
            [](const auto& l, const auto& r) {
              return l + ", " + std::to_string(r);
            });

        TRACE(SBCC, 2, "    %s:\n      %s", meth_name.c_str(),
              idListStr.c_str());
      }
    }

    return num_missing_blks;
  }

  return 0;
}
} // namespace source_blocks
