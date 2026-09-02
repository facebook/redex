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

constexpr const char* kHolder =
    "Lcom/facebook/redextest/AtomicFieldUpdaterLowering$Holder;";

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

// A provably non-null holder lowers with no guard: the updater calls are gone
// and the corresponding Unsafe primitives are in their place.
//
// `compareAndSet` is deliberately not asserted on here. D8 rewrites the
// reference-flavored one into a `$$ExternalSyntheticBackportWithForwarding0`
// forwarder that receives the updater as a parameter, so at this call site it
// is no longer an updater operation at all. That is a property of desugaring,
// not of the pass; `primitives` below covers compare-and-set on the Integer
// flavor, which D8 leaves alone.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, lowersProvenHolder) {
  run();
  auto names = invoked_in("provenHolder:()Ljava/lang/Object;");
  EXPECT_EQ(names.count("getObjectVolatile"), 1);
  EXPECT_EQ(names.count("get"), 0);
  // `set(h, "a")` writes a String into an Object-typed field. The value
  // obligation cannot lean on `check_cast` here: that walks the hierarchy via
  // `type_class`, which is null for a framework type no dex defines, so it
  // answers "not provably castable" for String. An Object field admits any
  // reference, and this is what checks the obligation knows it.
  EXPECT_EQ(names.count("putObjectVolatile"), 1);
  EXPECT_EQ(names.count("set"), 0);
  EXPECT_EQ(names.count("checkHolder"), 0) << "holder is a fresh instance";
}

// Each offset lives on the class declaring the field, not on the shared
// synthetic class: a holder is often not public, and reflecting on it from
// another package throws IllegalAccessError as that class initializes. Only
// an end-to-end run over real classes can show the placement is right.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, offsetsLiveOnTheDeclaringClass) {
  run();
  auto* holder = type_class(DexType::get_type(kHolder));
  ASSERT_NE(holder, nullptr);
  size_t offsets = 0;
  for (auto* f : holder->get_sfields()) {
    if (show(f->get_type()) == "J") {
      offsets++;
    }
  }
  EXPECT_EQ(offsets, 3) << "one offset per updater the holder declares";

  auto* synth =
      type_class(DexType::get_type("Lredex/AtomicFieldUpdaterUnsafe;"));
  ASSERT_NE(synth, nullptr);
  for (auto* f : synth->get_sfields()) {
    EXPECT_NE(show(f->get_type()), "J")
        << "offset " << show(f) << " must not live on the shared class";
  }
}

// A holder that cannot be proven non-null is still lowered, behind a check
// that reproduces the ClassCastException `accessCheck` would have thrown.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, guardsUnprovenHolder) {
  run();
  auto names = invoked_in(
      "unprovenHolder:(Lcom/facebook/redextest/"
      "AtomicFieldUpdaterLowering$Holder;)Ljava/lang/Object;");
  EXPECT_EQ(names.count("getObjectVolatile"), 1);
  EXPECT_EQ(names.count("checkHolder"), 1);
  EXPECT_GE(metric("null_checks_emitted"), 1);
}

TEST_F(AtomicFieldUpdaterLoweringIntegTest, lowersIntegerAndLong) {
  run();
  auto names = invoked_in("primitives:()J");
  EXPECT_EQ(names.count("getIntVolatile"), 1);
  EXPECT_EQ(names.count("putIntVolatile"), 1);
  EXPECT_EQ(names.count("compareAndSwapInt"), 1);
  EXPECT_EQ(names.count("getLongVolatile"), 1);
  EXPECT_EQ(names.count("putLongVolatile"), 1);
}
