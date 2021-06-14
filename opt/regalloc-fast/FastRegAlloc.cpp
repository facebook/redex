/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FastRegAlloc.h"

#include "Trace.h"

namespace fastregalloc {

void FastRegAllocPass::eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) {
  ++m_eval;
}

void FastRegAllocPass::run_pass(DexStoresVector& stores,
                                ConfigFiles&,
                                PassManager& mgr) {
  TRACE(FREG, 1, "FastRegAllocPass reached!");
  ++m_run;
}

static FastRegAllocPass s_pass;

} // namespace fastregalloc
