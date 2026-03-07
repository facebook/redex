/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "KotlinCompanionOptimizationPass.h"
#include "LocalDcePass.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"

class KotlinCompanionOptimizationTest : public RedexIntegrationTest {
 protected:
  void dump_cls(DexClass* cls) {
    TRACE(KOTLIN_COMPANION, 5, "Class %s", SHOW(cls));
    std::vector<DexMethod*> methods = cls->get_all_methods();
    std::vector<DexField*> fields = cls->get_all_fields();
    for (auto* v : fields) {
      TRACE(KOTLIN_COMPANION, 5, "Field %s", SHOW(v));
    }
    for (auto* v : methods) {
      TRACE(KOTLIN_COMPANION, 5, "Method %s", SHOW(v));
      TRACE(KOTLIN_COMPANION, 5, "%s", SHOW(v->get_code()));
    }
  }

  void set_root_method(const std::string& full_name) {
    auto* method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }
};
namespace {

// Test basic companion optimization: companion object methods are relocated
// to the outer class as static methods and virtual calls are rewritten to
// static calls.
//
// Input (KotlinCompanionOptimization.kt):
//   CompanionClass has a companion with getSomeStr (property accessor)
//   AnotherCompanionClass has a named companion "Test" with @JvmStatic
//     getSomeOtherStr and funX
//
// After optimization, Foo.main() should contain only static calls into
// the outer classes (no virtual calls through the companion instance).
TEST_F(KotlinCompanionOptimizationTest, CompanionMethodsRelocatedToStatic) {
  auto scope = build_class_scope(stores);
  set_root_method("Lcom/facebook/redextest/objtest/Foo;.main:()V");
  auto* main_method =
      DexMethod::get_method("Lcom/facebook/redextest/objtest/Foo;.main:()V")
          ->as_def();
  auto* codex = main_method->get_code();
  ASSERT_NE(nullptr, codex);

  auto klr = std::make_unique<KotlinCompanionOptimizationPass>();
  auto dce = std::make_unique<LocalDcePass>();
  std::vector<Pass*> passes{klr.get(), dce.get()};
  run_passes(passes);
  DexType* main = DexType::get_type("Lcom/facebook/redextest/objtest/Foo;");
  DexType* outer1 =
      DexType::get_type("Lcom/facebook/redextest/objtest/CompanionClass;");
  DexType* outer2 = DexType::get_type(
      "Lcom/facebook/redextest/objtest/AnotherCompanionClass;");
  dump_cls(type_class(main));
  dump_cls(type_class(outer1));
  dump_cls(type_class(outer2));
  auto iterable = InstructionIterable(codex);
  unsigned static_calls = 0;
  std::unordered_set<DexType*> outer_classes;
  outer_classes.insert(outer1);
  outer_classes.insert(outer2);
  for (const auto& mie : iterable) {
    auto* insn = mie.insn;
    if (!opcode::is_an_invoke(insn->opcode())) {
      continue;
    }
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      static_calls++;
      DexType* cls = insn->get_method()->get_class();
      ASSERT_EQ(outer_classes.count(cls), 1);
    }
  }
  // After optimization, there should be exactly 3 static calls in Foo.main():
  //   1. CompanionClass.getSomeStr (property accessor for someStr)
  //   2. AnotherCompanionClass.getSomeOtherStr (property accessor)
  //   3. AnotherCompanionClass.funX
  ASSERT_EQ(static_calls, 3);
}

// Test method name collision: when the outer class and its companion both
// define a method with the same name (get()), the pass must rename the
// relocated companion method to avoid a conflict.
//
// Input (KotlinCompanionOptimization.kt — CompanionWithMethodCollision):
//   CompanionWithMethodCollision.get() returns "test1"  (outer class method)
//   CompanionWithMethodCollision.Companion.get() returns "test2"  (companion)
//
// After optimization, the companion's get() is relocated to the outer class
// as a static method renamed to "get$CompanionWithMethodCollision$Companion"
// to avoid colliding with the outer class's existing get().
TEST_F(KotlinCompanionOptimizationTest, MethodNameCollisionRenaming) {
  auto scope = build_class_scope(stores);
  set_root_method(
      "Lcom/facebook/redextest/objtest/CollisionTestCaller;.main:()V");
  auto* main_method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/objtest/CollisionTestCaller;.main:()V")
          ->as_def();
  auto* codex = main_method->get_code();
  ASSERT_NE(nullptr, codex);

  auto klr = std::make_unique<KotlinCompanionOptimizationPass>();
  auto dce = std::make_unique<LocalDcePass>();
  std::vector<Pass*> passes{klr.get(), dce.get()};
  run_passes(passes);
  DexType* main =
      DexType::get_type("Lcom/facebook/redextest/objtest/CollisionTestCaller;");
  DexType* outer1 = DexType::get_type(
      "Lcom/facebook/redextest/objtest/CompanionWithMethodCollision;");
  dump_cls(type_class(main));
  dump_cls(type_class(outer1));
  auto iterable = InstructionIterable(codex);
  unsigned static_calls = 0;
  std::unordered_set<DexType*> outer_classes;
  outer_classes.insert(outer1);
  for (const auto& mie : iterable) {
    auto* insn = mie.insn;
    if (!opcode::is_an_invoke(insn->opcode())) {
      continue;
    }
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      static_calls++;
      DexType* cls = insn->get_method()->get_class();
      // The relocated method is renamed to avoid colliding with the outer
      // class's own get() method.
      ASSERT_EQ(insn->get_method()->get_name()->str(),
                "get$CompanionWithMethodCollision$Companion");
      ASSERT_EQ(outer_classes.count(cls), 1);
    }
  }
  // Exactly one static call: the renamed companion get().
  // The outer class's own get() is called via invoke-virtual, not static.
  ASSERT_EQ(static_calls, 1);
}
} // namespace
