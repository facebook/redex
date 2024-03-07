/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintKotlinStats.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "PassManager.h"
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

  ASSERT_EQ(stats.kotlin_null_check_insns, 2);
}
