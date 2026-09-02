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
  return "Lcom/facebook/redextest/AtomicFieldUpdaterHelperPattern;." + sig;
}

std::set<std::string> invoked_in(const std::string& sig) {
  std::set<std::string> names;
  auto* mref = DexMethod::get_method(method_name(sig));
  auto* m = mref == nullptr ? nullptr : mref->as_def();
  auto* code = m == nullptr ? nullptr : m->get_code();
  if (code == nullptr) {
    return names;
  }
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

class AtomicFieldUpdaterHelperPatternIntegTest : public RedexIntegrationTest {
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

TEST_F(AtomicFieldUpdaterHelperPatternIntegTest,
       helperObjectPatternIsLeftAlone) {
  run();

  auto names = invoked_in(
      "helperFieldPattern:(Lcom/facebook/redextest/"
      "AtomicFieldUpdaterHelperPattern$Holder;)"
      "Ljava/lang/Object;");
  EXPECT_EQ(names.count("get"), 1);
  EXPECT_EQ(names.count("getObjectVolatile"), 0);
  EXPECT_EQ(metric("ops_total"), 1);
  EXPECT_EQ(metric("updaters_recognized"), 0);
  EXPECT_EQ(type_class(DexType::get_type("Lredex/AtomicFieldUpdaterUnsafe;")),
            nullptr);
}
