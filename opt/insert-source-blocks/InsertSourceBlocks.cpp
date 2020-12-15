/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <atomic>
#include <cstdint>

#include "DexClass.h"
#include "DexStore.h"
#include "IRCode.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "SourceBlocks.h"
#include "Walkers.h"

using namespace cfg;

namespace {

size_t source_blocks(DexMethod* method, IRCode* code) {
  ScopedCFG cfg(code);
  return source_blocks::insert_source_blocks(method, cfg.get());
}

void run_source_blocks(DexStoresVector& stores, PassManager& mgr) {
  auto scope = build_class_scope(stores);

  std::atomic<size_t> all{0};
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code != nullptr) {
      auto s = source_blocks(method, code);
      all.fetch_add(s);
    }
  });
  mgr.set_metric("inserted_source_blocks", all.load());
}

} // namespace

void InsertSourceBlocksPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles&,
                                      PassManager& mgr) {
  run_source_blocks(stores, mgr);
}

static InsertSourceBlocksPass s_pass;
