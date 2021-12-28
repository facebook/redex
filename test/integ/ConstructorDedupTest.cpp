/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"
#include "IRTypeChecker.h"
#include "NormalizeConstructor.h"
#include "RedexTest.h"
#include "Show.h"
#include "Walkers.h"

class ConstructorDedupTest : public RedexIntegrationTest {};

TEST_F(ConstructorDedupTest, dedup) {
  auto type =
      DexType::get_type("Lcom/facebook/redextest/ConstructorDedupTest;");
  auto cls = type_class(type);
  EXPECT_NE(cls, nullptr);
  std::vector<DexClass*> scope({cls});
  auto ctors = cls->get_ctors();
  auto dedupped = method_dedup::dedup_constructors(scope, scope);
  EXPECT_EQ(dedupped, 6);
  walk::parallel::methods(scope, [&](DexMethod* method) {
    IRTypeChecker checker(method);
    checker.run();
    if (checker.fail()) {
      std::string msg = checker.what();
      fprintf(stderr, "ABORT! Inconsistency found in Dex code for %s.\n %s\n",
              SHOW(method), msg.c_str());
      fprintf(stderr, "Code:\n%s\n", SHOW(method->get_code()->cfg()));
      exit(EXIT_FAILURE);
    }
    auto method_name = method->str();
    if (method_name.find("dedup_") == std::string::npos) {
      return;
    }
    DexMethod* ctor{nullptr};
    if (method_name == "dedup_0") {
      // All the constructor invocations are calling ctors[0].
      ctor = ctors[0];
    } else if (method_name == "dedup_1") {
      // All the constructor invocations are calling ctors[1] - the proto also
      // contains additional integer parameters to address collisions
      ctor = ctors[1];
    } else {
      // All the constructor invocations are calling ctors[2].
      ctor = ctors[2];
    }

    for (auto& mie : InstructionIterable(method->get_code())) {
      auto insn = mie.insn;
      if (insn->has_method()) {
        auto callee = insn->get_method();
        if (callee->get_class() == type && method::is_init(callee)) {
          // Only one constructor is used.
          EXPECT_EQ(callee, ctor);
        }
      }
    }
  });
}
