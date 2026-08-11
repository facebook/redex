/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "AtomicFieldUpdaterLoweringPass.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"

namespace {

std::string method_name(const std::string& sig) {
  return "Lcom/facebook/redextest/AtomicFieldUpdaterLowering;." + sig;
}

// Names of the methods invoked in `sig`, so a test can say what the rewrite
// produced without depending on register allocation or instruction order.
std::set<std::string> invoked_in(const std::string& sig) {
  std::set<std::string> names;
  auto* mref = DexMethod::get_method(method_name(sig));
  auto* m = mref == nullptr ? nullptr : mref->as_def();
  auto* code = m == nullptr ? nullptr : m->get_code();
  if (code == nullptr) {
    return names;
  }
  // The CFG state after `run_passes` is not part of the contract; ScopedCFG
  // builds one only if there is not one already.
  cfg::ScopedCFG scoped(code);
  for (auto& mie : cfg::InstructionIterable(*scoped)) {
    auto* insn = mie.insn;
    if (insn->has_method()) {
      names.insert(show(insn->get_method()->get_name()));
    }
  }
  return names;
}

} // namespace

class AtomicFieldUpdaterLoweringIntegTest : public RedexIntegrationTest {
 protected:
  int64_t metric(const std::string& key) {
    for (const auto& info : pass_manager->get_pass_info()) {
      if (info.name.find("AtomicFieldUpdaterLowering") == std::string::npos) {
        continue;
      }
      auto it = info.metrics.find(key);
      if (it != info.metrics.end()) {
        return it->second;
      }
    }
    return -1;
  }

  void run() {
    std::vector<Pass*> passes{new AtomicFieldUpdaterLoweringPass()};
    run_passes(passes);
  }
};

// The three flavors are recognized from real javac output, where `newUpdater`
// is called in <clinit> exactly as an app would write it. The unit tests
// assemble that shape by hand and so cannot show it survives a real compile.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, recognizesEveryFlavor) {
  run();
  EXPECT_EQ(metric("updaters_recognized"), 3);
  EXPECT_EQ(metric("updaters_recognized_reference"), 1);
  EXPECT_EQ(metric("updaters_recognized_integer"), 1);
  EXPECT_EQ(metric("updaters_recognized_long"), 1);

  // The census counts every call against an updater type, including the two
  // inside the forwarder D8 synthesizes for the reference compareAndSet.
  EXPECT_EQ(metric("ops_total"), 11);
}

// An updater passed in as a parameter has no field to resolve to, so the call
// is left exactly as it was.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, leavesUnresolvableUpdaterAlone) {
  run();
  auto names = invoked_in(
      "unresolvableUpdater:(Ljava/util/concurrent/atomic/"
      "AtomicReferenceFieldUpdater;Lcom/facebook/redextest/"
      "AtomicFieldUpdaterLowering$Holder;)Ljava/lang/Object;");
  EXPECT_EQ(names.count("get"), 1);
  EXPECT_EQ(names.count("getObjectVolatile"), 0);
  // Not an exact total: D8's compareAndSet forwarder is a second, incidental
  // instance of the same shape, and pinning the count here would make the test
  // a record of desugaring behavior.
  EXPECT_GE(metric("calls_skipped_unresolved_updater"), 1);
}
