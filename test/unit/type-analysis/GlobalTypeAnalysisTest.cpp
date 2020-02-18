/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalyzer.h"

#include "CallGraph.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "Walkers.h"

using namespace type_analyzer;
using namespace type_analyzer::global;

struct GlobalTypeAnalysisTest : public RedexTest {
 public:
  GlobalTypeAnalysisTest() {
    auto cls_o = DexType::make_type("LO;");
    ClassCreator creator(cls_o);
    creator.set_super(type::java_lang_Object());

    auto m_init = assembler::method_from_string(R"(
      (method (public static) "LO;.<init>:()V"
       (
        (return-void)
       )
      )
    )");
    creator.add_method(m_init);
    m_cls_o = creator.create();
  }

 protected:
  void prepare_scope(Scope& scope) { scope.push_back(m_cls_o); }

  DexTypeDomain get_type_domain(const std::string& type_name) {
    return DexTypeDomain(DexType::make_type(DexString::make_string(type_name)));
  }

  DexClass* m_cls_o;
};

TEST_F(GlobalTypeAnalysisTest, SimpleArgumentPassingTest) {
  Scope scope;
  prepare_scope(scope);

  auto cls_a = DexType::make_type("LA;");
  ClassCreator creator(cls_a);
  creator.set_super(type::java_lang_Object());

  auto meth_bar = assembler::method_from_string(R"(
    (method (public static) "LA;.bar:(LO;)V"
     (
      (load-param-object v0)
      (return-void)
     )
    )
  )");
  creator.add_method(meth_bar);

  auto meth_foo = assembler::method_from_string(R"(
    (method (public static) "LA;.foo:()V"
     (
      (new-instance "LO;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LO;.<init>:()V")
      (invoke-static (v0) "LA;.bar:(LO;)V")
      (return-void)
     )
    )
  )");
  meth_foo->rstate.set_root();
  creator.add_method(meth_foo);
  scope.push_back(creator.create());

  call_graph::Graph cg = call_graph::single_callee_graph(scope);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });
  GlobalTypeAnalyzer gta(cg);
  gta.run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
  auto foo_arg_env =
      gta.get_entry_state_at(meth_foo).get(CURRENT_PARTITION_LABEL);
  EXPECT_TRUE(foo_arg_env.is_top());
  auto bar_arg_env =
      gta.get_entry_state_at(meth_bar).get(CURRENT_PARTITION_LABEL);
  EXPECT_EQ(bar_arg_env,
            ArgumentTypeEnvironment({{0, get_type_domain("LO;")}}));
}

TEST_F(GlobalTypeAnalysisTest, ArgumentPassingJoinWithNullTest) {
  Scope scope;
  prepare_scope(scope);

  auto cls_a = DexType::make_type("LA;");
  ClassCreator creator(cls_a);
  creator.set_super(type::java_lang_Object());

  auto meth_bar = assembler::method_from_string(R"(
    (method (public static) "LA;.bar:(LO;LO;)V"
     (
      (load-param-object v0)
      (load-param-object v1)
      (return-void)
     )
    )
  )");
  creator.add_method(meth_bar);

  auto meth_foo = assembler::method_from_string(R"(
    (method (public static) "LA;.foo:()V"
     (
      (const v0 0)
      (const v1 0)
      (new-instance "LO;")
      (move-result-pseudo-object v2)
      (invoke-direct (v2) "LO;.<init>:()V")

      (if-eqz v0 :lb0)
      (new-instance "LO;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LO;.<init>:()V")
      (goto :lb0)

      (:lb0)
      (invoke-static (v1 v2) "LA;.bar:(LO;LO;)V")
      (return-void)
     )
    )
  )");
  meth_foo->rstate.set_root();
  creator.add_method(meth_foo);
  scope.push_back(creator.create());

  call_graph::Graph cg = call_graph::single_callee_graph(scope);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });
  GlobalTypeAnalyzer gta(cg);
  gta.run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
  auto foo_arg_env =
      gta.get_entry_state_at(meth_foo).get(CURRENT_PARTITION_LABEL);
  EXPECT_TRUE(foo_arg_env.is_top());
  auto bar_arg_env =
      gta.get_entry_state_at(meth_bar).get(CURRENT_PARTITION_LABEL);
  EXPECT_TRUE(bar_arg_env.get(0).is_top());
  EXPECT_EQ(bar_arg_env,
            ArgumentTypeEnvironment({{1, get_type_domain("LO;")}}));
}
