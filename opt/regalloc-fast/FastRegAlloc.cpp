/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FastRegAlloc.h"

#include "DexUtil.h"
#include "LinearScan.h"
#include "Trace.h"
#include "Walkers.h"

namespace fastregalloc {

void FastRegAllocPass::eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) {
  ++m_eval;
}

void FastRegAllocPass::run_pass(DexStoresVector& stores,
                                ConfigFiles&,
                                PassManager& mgr) {
  TRACE(FREG, 1, "FastRegAllocPass reached!");
  auto scope = build_class_scope(stores);
  walk::parallel::methods(scope, [&](DexMethod* m) {
    LinearScanAllocator allocator(m);
    allocator.allocate();
  });
  ++m_run;
}

static FastRegAllocPass s_pass;

} // namespace fastregalloc
