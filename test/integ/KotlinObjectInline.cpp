/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "KotlinObjectInliner.h"
#include "LocalDcePass.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"

class KotlinLambdaOptTest : public RedexIntegrationTest {
 protected:
  void dump_cls(DexClass* cls) {
    TRACE(KOTLIN_OBJ_INLINE, 5, "Class %s", SHOW(cls));
    std::vector<DexMethod*> methods = cls->get_all_methods();
    std::vector<DexField*> fields = cls->get_all_fields();
    for (auto* v : fields) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Field %s", SHOW(v));
    }
    for (auto* v : methods) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Method %s", SHOW(v));
      TRACE(KOTLIN_OBJ_INLINE, 5, "%s", SHOW(v->get_code()));
    }
  }

  void set_root_method(const std::string& full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }
};
namespace {

TEST_F(KotlinLambdaOptTest, MethodHasNoEqDefined) {
  auto scope = build_class_scope(stores);
  set_root_method("Lcom/facebook/redextest/objtest/Foo;.main:()V");
  auto main_method =
      DexMethod::get_method("Lcom/facebook/redextest/objtest/Foo;.main:()V")
          ->as_def();
  auto codex = main_method->get_code();
  ASSERT_NE(nullptr, codex);

  auto klr = std::make_unique<KotlinObjectInliner>();
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
    auto insn = mie.insn;
    if (!opcode::is_an_invoke(insn->opcode())) {
      continue;
    }
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      static_calls++;
      DexType* cls = insn->get_method()->get_class();
      ASSERT_EQ(outer_classes.count(cls), 1);
    }
  }
  ASSERT_EQ(static_calls, 2);
}

TEST_F(KotlinLambdaOptTest, MethodCollideTest) {
  auto scope = build_class_scope(stores);
  set_root_method("Lcom/facebook/redextest/objtestjava/FooJava;.main:()V");
  auto main_method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/objtestjava/FooJava;.main:()V")
          ->as_def();
  auto codex = main_method->get_code();
  ASSERT_NE(nullptr, codex);

  auto klr = std::make_unique<KotlinObjectInliner>();
  auto dce = std::make_unique<LocalDcePass>();
  std::vector<Pass*> passes{klr.get(), dce.get()};
  run_passes(passes);
  DexType* main =
      DexType::get_type("Lcom/facebook/redextest/objtestjava/FooJava;");
  DexType* outer1 = DexType::get_type(
      "Lcom/facebook/redextest/objtestjava/KotlinCompanionObj;");
  dump_cls(type_class(main));
  dump_cls(type_class(outer1));
  auto iterable = InstructionIterable(codex);
  unsigned static_calls = 0;
  std::unordered_set<DexType*> outer_classes;
  outer_classes.insert(outer1);
  for (const auto& mie : iterable) {
    auto insn = mie.insn;
    if (!opcode::is_an_invoke(insn->opcode())) {
      continue;
    }
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      static_calls++;
      DexType* cls = insn->get_method()->get_class();
      // Name get() is renamed
      ASSERT_EQ(insn->get_method()->get_name()->str(),
                "get$KotlinCompanionObj$Companion");
      ASSERT_EQ(outer_classes.count(cls), 1);
    }
  }
  ASSERT_EQ(static_calls, 1);
}
} // namespace
