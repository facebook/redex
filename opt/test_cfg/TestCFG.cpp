/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TestCFG.h"

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "ParallelWalkers.h"
#include "Walkers.h"

std::vector<int64_t> get_lits(IRCode* code) {
  std::vector<int64_t> result;
  for (const auto& mie : InstructionIterable(code)) {
    if (mie.insn->has_literal()) {
      result.push_back(mie.insn->get_literal());
    }
  }
  return result;
}

/**
 * This isn't a real optimization pass. It just tests the CFG.
 * this should only run in redex-unstable
 */
void TestCFGPass::run_pass(DexStoresVector& stores,
                           ConfigFiles& /* unused */,
                           PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  walk_methods_parallel_simple(scope, [](DexMethod* m) {
    IRCode* code = m->get_code();

    if (code == nullptr) {
      return;
    }

    const auto& before_lits = get_lits(code);

    // build and linearize the CFG
    code->build_cfg(/* editable */ true);
    TRACE(CFG, 5, "  cfg build done\n");
    code->clear_cfg();
    TRACE(CFG, 5, "  cfg linearize done\n");
    TRACE(CFG, 5, "fm after:\n%s", SHOW(code));

    // Run the IR type checker
    IRTypeChecker checker(m);
    checker.run();
    if (!checker.good()) {
      std::string msg = checker.what();
      TRACE(
          CFG, 1, "%s: Inconsistency in Dex code. %s\n", SHOW(m), msg.c_str());
      always_assert(false);
    }

    const auto& after_lits = get_lits(code);

    size_t i = 0;
    for (auto v : after_lits) {
      always_assert_log(v == before_lits[i],
                        "%s: literal changed from %ld to %ld. Code:\n%s",
                        SHOW(m),
                        v,
                        before_lits[i],
                        SHOW(code));
      ++i;
    }
  });
}

TestCFGPass s_pass;
