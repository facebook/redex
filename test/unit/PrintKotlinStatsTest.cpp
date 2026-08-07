/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintKotlinStats.h"
#include "AtomicFieldUpdaters.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "IRTemplate.h"
#include "RedexTest.h"
#include "Walkers.h"

struct PrintKotlinStatsTest : public RedexTest {
 public:
  PrintKotlinStatsTest() {
    m_cls_public = DexType::make_type("LPUB;");
    m_init1 = assembler::method_from_string(R"(
      (method (public constructor) "LPUB;.<init>:()V"
       (
        (return-void)
       )
      )
    )");

    m_cls_private = DexType::make_type("LPRI;");
    m_init2 = assembler::method_from_string(R"(
      (method (public constructor) "LPRI;.<init>:()V"
       (
        (return-void)
       )
      )
    )");
  }

 protected:
  void prepare_scope(Scope& scope,
                     DexMethod* method_public,
                     DexMethod* method_private) {
    ClassCreator creator1(m_cls_public);
    creator1.set_super(type::java_lang_Object());
    ClassCreator creator2(m_cls_private);
    creator2.set_super(type::java_lang_Object());

    creator1.add_method(m_init1);
    creator1.add_method(method_public);
    m_m_cls_public = creator1.create();

    creator2.add_method(m_init2);
    creator2.add_method(method_private);
    m_m_cls_private = creator2.create();

    scope.push_back(m_m_cls_public);
    scope.push_back(m_m_cls_private);
  }
  DexClass* m_m_cls_public;
  DexClass* m_m_cls_private;
  DexType* m_cls_public;
  DexType* m_cls_private;
  DexMethod* m_init1;
  DexMethod* m_init2;
};

TEST_F(PrintKotlinStatsTest, SimpleArgumentPassingTest) {
  Scope scope;
  DexMethod* method_public = assembler::method_from_string(R"(
      (method (public) "LPUB;.meth1:(Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;"
       (
        (load-param-object v0)
        (const-string "args")
        (move-result-pseudo-object v1)
        (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
        (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
        (return-object v1)
       )
      )
    )");
  DexMethod* method_private = assembler::method_from_string(R"(
      (method (private) "LPRI;.meth2:(Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;"
       (
        (return-object v1)
       )
      )
    )");

  prepare_scope(scope, method_public, method_private);
  PrintKotlinStats pass;
  pass.setup();
  PrintKotlinStats::Stats stats =
      walk::parallel::methods<PrintKotlinStats::Stats>(
          scope, [&](DexMethod* meth) {
            meth->get_code()->build_cfg();
            return pass.handle_method(meth);
          });

  ASSERT_EQ(stats.kotlin_null_check_param_insns_in_root_method, 0);
  ASSERT_EQ(stats.kotlin_null_check_param_insns_in_non_root_method, 1);
  ASSERT_EQ(stats.kotlin_null_check_expr_insns, 1);
  ASSERT_EQ(stats.kotlin_null_check_notnull_insns, 0);
}

TEST_F(PrintKotlinStatsTest, CheckNotNullTest) {
  Scope scope;
  DexMethod* method_public = assembler::method_from_string(R"(
      (method (public) "LPUB;.meth1:(Ljava/lang/Object;)Ljava/lang/Object;"
       (
        (load-param-object v0)
        (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.checkNotNull:(Ljava/lang/Object;)V")
        (const-string "null cannot be cast to non-null type kotlin.String")
        (move-result-pseudo-object v1)
        (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.checkNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
        (return-object v0)
       )
      )
    )");
  DexMethod* method_private = assembler::method_from_string(R"(
      (method (private) "LPRI;.meth2:(Ljava/lang/Object;)Ljava/lang/Object;"
       (
        (return-object v1)
       )
      )
    )");

  prepare_scope(scope, method_public, method_private);
  PrintKotlinStats pass;
  pass.setup();
  PrintKotlinStats::Stats stats =
      walk::parallel::methods<PrintKotlinStats::Stats>(
          scope, [&](DexMethod* meth) {
            meth->get_code()->build_cfg();
            return pass.handle_method(meth);
          });

  ASSERT_EQ(stats.kotlin_null_check_param_insns_in_root_method, 0);
  ASSERT_EQ(stats.kotlin_null_check_param_insns_in_non_root_method, 0);
  ASSERT_EQ(stats.kotlin_null_check_expr_insns, 0);
  ASSERT_EQ(stats.kotlin_null_check_notnull_insns, 2);
}

TEST_F(PrintKotlinStatsTest, AtomicFieldUpdaterTest) {
  Scope scope;
  // One newUpdater() allocation and one operation call site (get).
  DexMethod* method_public = assembler::method_from_string(
      ir(R"(
      (method (public) "LPUB;.meth1:(Ljava/lang/Object;)Ljava/lang/Object;"
       (
        (load-param-object v0)
        (const-class "LPUB;")
        (move-result-pseudo-object v1)
        (const-class "Ljava/lang/Object;")
        (move-result-pseudo-object v2)
        (const-string "f")
        (move-result-pseudo-object v3)
        (invoke-static (v1 v2 v3) "$REF.newUpdater:(Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/String;)$REF")
        (move-result-object v4)
        (invoke-virtual (v4 v0) "$REF.get:(Ljava/lang/Object;)Ljava/lang/Object;")
        (move-result-object v5)
        (return-object v5)
       )
      )
    )",
         {{"$REF", atomic_field_updaters::REFERENCE_DESC},
          {"$INT", atomic_field_updaters::INTEGER_DESC},
          {"$LONG", atomic_field_updaters::LONG_DESC}}));
  DexMethod* method_private = assembler::method_from_string(R"(
      (method (private) "LPRI;.meth2:(Ljava/lang/Object;)Ljava/lang/Object;"
       (
        (return-object v1)
       )
      )
    )");

  prepare_scope(scope, method_public, method_private);
  PrintKotlinStats pass;
  pass.setup();
  PrintKotlinStats::Stats stats =
      walk::parallel::methods<PrintKotlinStats::Stats>(
          scope, [&](DexMethod* meth) {
            meth->get_code()->build_cfg();
            return pass.handle_method(meth);
          });

  ASSERT_EQ(stats.atomic_field_updater_newupdater_insns, 1);
  ASSERT_EQ(stats.atomic_field_updater_op_insns, 1);
}

// The counters cover all three flavors, not just the reference one. The
// Integer and Long variants take `newUpdater(Class, String)` -- no value class
// -- so a counter keyed on the reference signature alone would miss them.
//
// Note there is deliberately no /range case here: Redex's IR has no range
// opcodes. `DOPCODE_INVOKE_VIRTUAL_RANGE` is mapped to `OPCODE_INVOKE_VIRTUAL`
// on load and the range form is re-selected at output time from the register
// layout, so a /range call site is indistinguishable here and is counted.
TEST_F(PrintKotlinStatsTest, AtomicFieldUpdaterAllFlavorsTest) {
  Scope scope;
  DexMethod* method_public = assembler::method_from_string(
      ir(R"(
      (method (public) "LPUB;.meth1:(Ljava/lang/Object;)I"
       (
        (load-param-object v0)
        (const-class "LPUB;")
        (move-result-pseudo-object v1)
        (const-string "i")
        (move-result-pseudo-object v2)
        (invoke-static (v1 v2) "$INT.newUpdater:(Ljava/lang/Class;Ljava/lang/String;)$INT")
        (move-result-object v3)
        (invoke-virtual (v3 v0) "$INT.get:(Ljava/lang/Object;)I")
        (move-result v4)
        (const-string "j")
        (move-result-pseudo-object v5)
        (invoke-static (v1 v5) "$LONG.newUpdater:(Ljava/lang/Class;Ljava/lang/String;)$LONG")
        (move-result-object v6)
        (const-wide v7 1)
        (invoke-virtual (v6 v0 v7) "$LONG.set:(Ljava/lang/Object;J)V")
        (invoke-virtual (v6 v0) "$LONG.getAndIncrement:(Ljava/lang/Object;)J")
        (move-result-wide v8)
        (return v4)
       )
      )
    )",
         {{"$REF", atomic_field_updaters::REFERENCE_DESC},
          {"$INT", atomic_field_updaters::INTEGER_DESC},
          {"$LONG", atomic_field_updaters::LONG_DESC}}));
  DexMethod* method_private = assembler::method_from_string(R"(
      (method (private) "LPRI;.meth2:(Ljava/lang/Object;)Ljava/lang/Object;"
       (
        (return-object v1)
       )
      )
    )");

  prepare_scope(scope, method_public, method_private);
  PrintKotlinStats pass;
  pass.setup();
  PrintKotlinStats::Stats stats =
      walk::parallel::methods<PrintKotlinStats::Stats>(
          scope, [&](DexMethod* meth) {
            meth->get_code()->build_cfg();
            return pass.handle_method(meth);
          });

  // One Integer and one Long newUpdater; three operations across the two
  // (get, set, getAndIncrement) -- including ops the lowering pass does not
  // handle, which is the point of counting separately.
  ASSERT_EQ(stats.atomic_field_updater_newupdater_insns, 2);
  ASSERT_EQ(stats.atomic_field_updater_op_insns, 3);
}
