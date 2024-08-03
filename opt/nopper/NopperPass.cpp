/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NopperPass.h"

#include <cmath>
#include <random>

#include "DexUtil.h"
#include "Nopper.h"
#include "PassManager.h"
#include "Walkers.h"

void NopperPass::eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) {}

void NopperPass::run_pass(DexStoresVector& stores,
                          ConfigFiles&,
                          PassManager& mgr) {
  mgr.record_running_nopper();

  if (m_probability == 0) {
    return;
  }

  auto scope = build_class_scope(stores);

  InsertOnlyConcurrentMap<DexMethod*, std::vector<cfg::Block*>>
      gathered_noppable_blocks;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode&) {
    gathered_noppable_blocks.emplace(
        method, nopper_impl::get_noppable_blocks(method->get_code()->cfg()));
  });

  std::vector<std::pair<DexMethod*, cfg::Block*>> noppable_blocks_vec;
  for (auto&& [method, blocks] : gathered_noppable_blocks) {
    for (auto* block : blocks) {
      noppable_blocks_vec.emplace_back(method, block);
    }
  }
  gathered_noppable_blocks.clear();

  std::mt19937 gen;
  gen.seed(0);
  std::shuffle(noppable_blocks_vec.begin(), noppable_blocks_vec.end(), gen);
  auto end = (size_t)std::lround(noppable_blocks_vec.size() * m_probability);
  noppable_blocks_vec.resize(end);

  std::unordered_map<DexMethod*, std::unordered_set<cfg::Block*>>
      noppable_blocks;
  for (auto&& [method, block] : noppable_blocks_vec) {
    noppable_blocks[method].insert(block);
  }

  std::atomic<size_t> nops_inserted{0};
  std::atomic<size_t> blocks{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    blocks.fetch_add(code.cfg().num_blocks());
    auto it = noppable_blocks.find(method);
    if (it == noppable_blocks.end()) {
      return;
    }
    auto local_nops_inserted = nopper_impl::insert_nops(code.cfg(), it->second);
    nops_inserted.fetch_add(local_nops_inserted);
  });

  mgr.set_metric("nops_inserted", nops_inserted.load());
  mgr.set_metric("blocks", blocks.load());
  TRACE(NOP,
        1,
        "%zu nops_inserted across %zu blocks",
        nops_inserted.load(),
        blocks.load());
}

static NopperPass s_pass;
