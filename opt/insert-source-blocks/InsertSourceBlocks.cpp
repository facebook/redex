/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <cstdint>
#include <fstream>
#include <mutex>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRCode.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

using namespace cfg;

namespace {

source_blocks::InsertResult source_blocks(DexMethod* method,
                                          IRCode* code,
                                          bool serialize) {
  ScopedCFG cfg(code);
  return source_blocks::insert_source_blocks(method, cfg.get(), serialize);
}

void run_source_blocks(DexStoresVector& stores,
                       ConfigFiles& conf,
                       PassManager& mgr,
                       bool serialize) {
  auto scope = build_class_scope(stores);

  std::mutex serialized_guard;
  std::vector<std::pair<const DexMethod*, std::string>> serialized;
  size_t blocks{0};
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code != nullptr) {
      auto res = source_blocks(method, code, serialize);
      std::unique_lock<std::mutex> lock(serialized_guard);
      serialized.emplace_back(method, std::move(res.serialized));
      blocks += res.block_count;
    }
  });
  mgr.set_metric("inserted_source_blocks", blocks);
  mgr.set_metric("handled_methods", serialized.size());

  if (!serialize) {
    return;
  }

  std::sort(serialized.begin(),
            serialized.end(),
            [](const auto& lhs, const auto& rhs) {
              return compare_dexmethods(lhs.first, rhs.first);
            });

  std::ofstream ofs(conf.metafile("redex-source-blocks.csv"));
  ofs << "type,version\nredex-source-blocks,1\nname,serialized\n";
  for (const auto& p : serialized) {
    ofs << show(p.first) << "," << p.second << "\n";
  }
}

} // namespace

void InsertSourceBlocksPass::bind_config() {
  bind("force_serialize",
       m_force_serialize_,
       m_force_serialize_,
       "Force serialization of the CFGs. Testing only.");
}

void InsertSourceBlocksPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  // TODO(agampe): This should eventually go away. For now, avoid the overhead.
  if (!mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(METH_PROF,
          1,
          "Not an instrumentation build, not running InsertSourceBlocksPass");
  }

  run_source_blocks(stores,
                    conf,
                    mgr,
                    mgr.get_redex_options().instrument_pass_enabled ||
                        m_force_serialize_);
}

static InsertSourceBlocksPass s_pass;
