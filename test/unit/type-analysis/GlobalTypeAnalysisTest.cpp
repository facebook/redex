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
      (method (public constructor) "LO;.<init>:()V"
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

  SingletonDexTypeDomain get_singleton_type_domain(
      const std::string& type_name) {
    return SingletonDexTypeDomain(
        DexType::make_type(DexString::make_string(type_name)));
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

  auto& graph = gta.get_call_graph();

  auto foo_arg_env =
      gta.get_entry_state_at(graph.node(meth_foo)).get(CURRENT_PARTITION_LABEL);
  EXPECT_TRUE(foo_arg_env.is_top());
  auto bar_arg_env =
      gta.get_entry_state_at(graph.node(meth_bar)).get(CURRENT_PARTITION_LABEL);
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

  auto& graph = gta.get_call_graph();

  auto foo_arg_env =
      gta.get_entry_state_at(graph.node(meth_foo)).get(CURRENT_PARTITION_LABEL);
  EXPECT_TRUE(foo_arg_env.is_top());
  auto bar_arg_env =
      gta.get_entry_state_at(graph.node(meth_bar)).get(CURRENT_PARTITION_LABEL);
  auto arg0 = bar_arg_env.get(0);
  EXPECT_FALSE(arg0.is_top());
  EXPECT_EQ(arg0.get_single_domain(), get_singleton_type_domain("LO;"));
  EXPECT_TRUE(arg0.is_nullable());
  auto arg1 = bar_arg_env.get(1);
  EXPECT_EQ(arg1, get_type_domain("LO;"));
}

TEST_F(GlobalTypeAnalysisTest, ReturnTypeTest) {
  Scope scope;
  prepare_scope(scope);

  auto cls_a = DexType::make_type("LA;");
  ClassCreator creator(cls_a);
  creator.set_super(type::java_lang_Object());

  auto meth_bar = assembler::method_from_string(R"(
    (method (public static) "LA;.bar:()LO;"
     (
      (new-instance "LO;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LO;.<init>:()V")
      (return-object v1)
     )
    )
  )");
  creator.add_method(meth_bar);

  auto meth_foo = assembler::method_from_string(R"(
    (method (public static) "LA;.foo:()V"
     (
      (invoke-static () "LA;.bar:()LO;")
      (move-result-object v0)
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

  GlobalTypeAnalysis analysis;
  auto gta = analysis.analyze(scope);
  auto wps = gta->get_whole_program_state();
  EXPECT_EQ(wps.get_return_type(meth_bar), get_type_domain("LO;"));

  auto lta = gta->get_local_analysis(meth_foo);
  auto code = meth_foo->get_code();
  auto bar_exit_env = lta->get_exit_state_at(code->cfg().exit_block());
  EXPECT_EQ(bar_exit_env.get_reg_environment().get(0), get_type_domain("LO;"));
}

TEST_F(GlobalTypeAnalysisTest, SimpleFieldTypeTest) {
  Scope scope;
  prepare_scope(scope);

  auto cls_a = DexType::make_type("LA;");
  ClassCreator creator(cls_a);
  creator.set_super(type::java_lang_Object());

  auto field_1 = DexField::make_field("LA;.f1:LO;")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_1);

  auto meth_init = assembler::method_from_string(R"(
    (method (public constructor) "LA;.<init>:()V"
     (
      (load-param-object v1) ; 'this' argument
      (new-instance "LO;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LO;.<init>:()V")
      (iput-object v0 v1 "LA;.f1:LO;")
      (return-void)
     )
    )
  )");
  creator.add_method(meth_init);

  auto meth_bar = assembler::method_from_string(R"(
    (method (public) "LA;.bar:()LO;"
     (
      (load-param-object v1) ; 'this' argument
      (iget-object v1 "LA;.f1:LO;")
      (move-result-pseudo-object v0)
      (return-object v0)
     )
    )
  )");
  creator.add_method(meth_bar);

  auto meth_foo = assembler::method_from_string(R"(
    (method (public static) "LA;.foo:()V"
     (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LA;.<init>:()V")
      (invoke-virtual (v0) "LA;.bar:()LO;")
      (move-result-object v1)
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

  GlobalTypeAnalysis analysis;
  auto gta = analysis.analyze(scope);
  auto wps = gta->get_whole_program_state();
  EXPECT_EQ(wps.get_field_type(field_1), get_type_domain("LO;"));
  EXPECT_EQ(wps.get_return_type(meth_bar), get_type_domain("LO;"));
  auto lta = gta->get_local_analysis(meth_foo);
  auto code = meth_foo->get_code();
  auto foo_exit_env = lta->get_exit_state_at(code->cfg().exit_block());
  EXPECT_EQ(foo_exit_env.get_reg_environment().get(1), get_type_domain("LO;"));
}

TEST_F(GlobalTypeAnalysisTest, ClinitSimpleTest) {
  Scope scope;
  prepare_scope(scope);

  auto cls_a = DexType::make_type("LA;");
  ClassCreator creator(cls_a);
  creator.set_super(type::java_lang_Object());

  auto field_1 = DexField::make_field("LA;.f1:LO;")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
  creator.add_field(field_1);

  auto meth_clinit = assembler::method_from_string(R"(
    (method (public static constructor) "LA;.<clinit>:()V"
     (
      (new-instance "LO;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LO;.<init>:()V")
      (iput-object v0 v1 "LA;.f1:LO;")
      (return-void)
     )
    )
  )");
  creator.add_method(meth_clinit);

  auto meth_init = assembler::method_from_string(R"(
    (method (public constructor) "LA;.<init>:()V"
     (
      (load-param-object v1) ; 'this' argument
      (return-void)
     )
    )
  )");
  creator.add_method(meth_init);

  auto meth_bar = assembler::method_from_string(R"(
    (method (public) "LA;.bar:()LO;"
     (
      (load-param-object v1) ; 'this' argument
      (iget-object v1 "LA;.f1:LO;")
      (move-result-pseudo-object v0)
      (return-object v0)
     )
    )
  )");
  creator.add_method(meth_bar);

  auto meth_foo = assembler::method_from_string(R"(
    (method (public static) "LA;.foo:()V"
     (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LA;.<init>:()V")
      (invoke-virtual (v0) "LA;.bar:()LO;")
      (move-result-object v1)
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

  GlobalTypeAnalysis analysis;
  auto gta = analysis.analyze(scope);
  auto wps = gta->get_whole_program_state();
  EXPECT_EQ(wps.get_field_type(field_1), get_type_domain("LO;"));
  EXPECT_EQ(wps.get_return_type(meth_bar), get_type_domain("LO;"));
  auto lta = gta->get_local_analysis(meth_foo);
  auto code = meth_foo->get_code();
  auto foo_exit_env = lta->get_exit_state_at(code->cfg().exit_block());
  EXPECT_EQ(foo_exit_env.get_reg_environment().get(1), get_type_domain("LO;"));
}
