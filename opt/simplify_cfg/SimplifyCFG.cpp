/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SimplifyCFG.h"

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "PassManager.h"
#include "Walkers.h"

void SimplifyCFGPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  auto total_insns_removed =
      walk::parallel::methods<int64_t>(scope, [](DexMethod* m) -> int64_t {
        auto code = m->get_code();
        if (code == nullptr) {
          return 0;
        }

        int64_t before_insns = code->count_opcodes();

        // build and linearize the CFG
        code->build_cfg(/* editable */ true);
        code->clear_cfg();

        int64_t after_insns = code->count_opcodes();
        return before_insns - after_insns;
      });
  mgr.set_metric("insns_removed", total_insns_removed);
}

static SimplifyCFGPass s_pass;
