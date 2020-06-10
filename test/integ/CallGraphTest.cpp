/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CallGraph.h"
#include "DexClass.h"
#include "RedexTest.h"

struct CallGraphTest : public RedexIntegrationTest {
 protected:
  DexMethod* clinit;
  DexMethod* calls_returns_int;
  DexMethod* base_returns_int;
  DexMethod* base_foo;
  DexMethod* extended_returns_int;

  Scope scope;
  boost::optional<call_graph::Graph> graph;

 public:
  CallGraphTest() {}
  void SetUp() override {
    scope = build_class_scope(stores);
    graph = call_graph::complete_call_graph(scope);
    clinit = DexMethod::get_method(
                 "Lcom/facebook/redextest/CallGraphTest;.<clinit>:()V")
                 ->as_def();

    calls_returns_int = DexMethod::get_method(
                            "Lcom/facebook/redextest/CallGraphTest;"
                            ".callsReturnsInt:(Lcom/facebook/redextest/Base;)I")
                            ->as_def();

    base_returns_int =
        DexMethod::get_method("Lcom/facebook/redextest/Base;.returnsInt:()I")
            ->as_def();
    base_foo = DexMethod::get_method("Lcom/facebook/redextest/Base;.foo:()I")
                   ->as_def();
    extended_returns_int =
        DexMethod::get_method(
            "Lcom/facebook/redextest/Extended;.returnsInt:()I")
            ->as_def();
  }
};

TEST_F(CallGraphTest, test_resolve_static_callees) {
  IRCode* code = clinit->get_code();
  IRInstruction* invoke_insn = nullptr;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && insn->get_method()->str() == "foo") {
      invoke_insn = mie.insn;
    }
  }
  ASSERT_NE(invoke_insn, nullptr);
  auto callees =
      call_graph::resolve_callees_in_graph(*graph, clinit, invoke_insn);
  EXPECT_EQ(callees.size(), 1);
  EXPECT_EQ(callees.count(base_foo), 1);
}

TEST_F(CallGraphTest, test_resolve_virtual_callees) {
  IRCode* code = calls_returns_int->get_code();
  IRInstruction* invoke_insn = nullptr;
  for (const auto& mie : InstructionIterable(code)) {
    if (is_invoke(mie.insn->opcode())) {
      invoke_insn = mie.insn;
    }
  }
  ASSERT_NE(invoke_insn, nullptr);
  auto callees = call_graph::resolve_callees_in_graph(
      *graph, calls_returns_int, invoke_insn);
  EXPECT_EQ(callees.size(), 2);
  EXPECT_EQ(callees.count(base_returns_int), 1);
  EXPECT_EQ(callees.count(extended_returns_int), 1);
}
