/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/algorithm/string/predicate.hpp>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "ImmutableSubcomponentAnalyzer.h"
#include "JarLoader.h"
#include "RedexTest.h"

std::vector<std::vector<std::string>> expected_paths = {
    // First call to `check`
    {"p0.getA()", "p0.getA().getB()", "p0.getA().getC()", "p0.getB().getD()",
     "p0.getA().getB().getD().getE()"},
    // Second call to `check`
    {"p1.getA()", "p1.getB()", "p1.getA().getC()", "p1.getB().getD()",
     "p1.getB().getD().getE()"},
    // Third call to `check`
    {"v11.getA()", "v11.getA().getB()", "v11.getA().getC()",
     "v11.getB().getD()", "v10"}};

void validate_arguments(size_t occurrence,
                        IRInstruction* insn,
                        const ImmutableSubcomponentAnalyzer& analyzer) {
  std::vector<std::string> args;
  for (size_t i = 0; i < 5; ++i) {
    auto path_opt = analyzer.get_access_path(insn->src(i), insn);
    args.push_back(path_opt ? path_opt->to_string() : "?");
  }
  EXPECT_THAT(args, ::testing::ElementsAreArray(expected_paths[occurrence]));
}

bool is_immutable_getter(DexMethodRef* method) {
  return boost::algorithm::starts_with(method->get_name()->str(), "get");
}

class ImmutableSubcomponentAnalyzerTest : public RedexIntegrationTest {};

TEST_F(ImmutableSubcomponentAnalyzerTest, accessPaths) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  for (const auto& cls : scope) {
    if (cls->get_name()->str() ==
        "Lcom/facebook/redextest/ImmutableSubcomponentAnalyzer;") {
      for (const auto& method : cls->get_dmethods()) {
        if (method->get_name()->str() == "test") {
          ImmutableSubcomponentAnalyzer analyzer(method, is_immutable_getter);
          size_t check_occurrence = 0;
          for (auto& mie : InstructionIterable(method->get_code())) {
            IRInstruction* insn = mie.insn;
            if (insn->opcode() == OPCODE_INVOKE_STATIC &&
                insn->get_method()->get_name()->str() == "check") {
              validate_arguments(check_occurrence++, insn, analyzer);
            }
          }
        }
      }
    }
  }
}

// Stub out another test method, but with IR so we know exactly which register
// numbers to use for assertions.
DexMethod* make_ir_test_method() {
  return assembler::method_from_string(R"(
    (method (private) "LFoo;.bar:(Lcom/facebook/Structure;)V"
     (
      (load-param-object v2) ; the `this` argument
      (load-param-object v3)
      (invoke-virtual (v3) "Lcom/facebook/Structure;.getA:()Lcom/facebook/A;")
      (move-result-object v0)
      (if-eqz v0 :label)
      (invoke-virtual (v3) "Lcom/facebook/Structure;.getA:()Lcom/facebook/A;")
      (move-result-object v1)
      (invoke-virtual (v1) "Lcom/facebook/A;.getB:()Lcom/facebook/B;")
      (move-result-object v0)
      (invoke-virtual (v2 v0) "LFoo;.baz:(Ljava/lang/Object;)V")
      (:label)
      (return-void)
     )
    )
  )");
}

TEST_F(ImmutableSubcomponentAnalyzerTest, findAccessPaths) {
  auto method = make_ir_test_method();
  auto code = method->get_code();
  ImmutableSubcomponentAnalyzer analyzer(method, is_immutable_getter);

  auto get_a = DexMethod::make_method(
      "Lcom/facebook/Structure;.getA:()Lcom/facebook/A;");
  auto get_b =
      DexMethod::make_method("Lcom/facebook/A;.getB:()Lcom/facebook/B;");

  auto found = false;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_method() && strcmp(insn->get_method()->c_str(), "baz") == 0) {
      found = true;
      {
        AccessPath p{AccessPathKind::Parameter, 1, {get_a}};
        auto path_str = p.to_string();
        EXPECT_EQ(path_str, "p1.getA()");
        auto reg = analyzer.find_access_path_registers(insn, p);
        ASSERT_EQ(reg.size(), 1);
        ASSERT_EQ(*reg.begin(), 1);
      }
      {
        std::vector<DexMethodRef*> getters;
        getters.emplace_back(get_a);
        getters.emplace_back(get_b);
        AccessPath p = AccessPath(AccessPathKind::Parameter, 1, getters);
        auto path_str = p.to_string();
        EXPECT_EQ(path_str, "p1.getA().getB()");
        auto reg = analyzer.find_access_path_registers(insn, p);
        ASSERT_EQ(reg.size(), 1);
        ASSERT_EQ(*reg.begin(), 0);
      }
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ImmutableSubcomponentAnalyzerTest, blockSnapshot) {
  auto method = make_ir_test_method();

  auto get_a = DexMethod::make_method(
      "Lcom/facebook/Structure;.getA:()Lcom/facebook/A;");
  auto get_b =
      DexMethod::make_method("Lcom/facebook/A;.getB:()Lcom/facebook/B;");

  AccessPath path_a{AccessPathKind::Parameter, 1, {get_a}};
  std::vector<DexMethodRef*> b_getters;
  b_getters.emplace_back(get_a);
  b_getters.emplace_back(get_b);
  auto path_b = AccessPath(AccessPathKind::Parameter, 1, b_getters);

  ImmutableSubcomponentAnalyzer analyzer(method, is_immutable_getter);
  auto snapshot = analyzer.get_block_state_snapshot();

  auto state0 = snapshot[0];
  EXPECT_EQ(state0.exit_state_bindings[0], path_a);
  auto state1 = snapshot[1];
  EXPECT_EQ(state1.entry_state_bindings[0], path_a);
  EXPECT_EQ(state1.exit_state_bindings[1], path_a);
  EXPECT_EQ(state1.exit_state_bindings[0], path_b);
  auto state2 = snapshot[2];
  EXPECT_EQ(state2.entry_state_bindings.find(0),
            state2.entry_state_bindings.end());
}

TEST_F(ImmutableSubcomponentAnalyzerTest, accessPathEquality) {
  AccessPath p0{AccessPathKind::Parameter, 0};
  AccessPath v0{AccessPathKind::Local, 0};
  EXPECT_NE(p0, v0);
  auto get_a = DexMethod::make_method(
      "Lcom/facebook/Structure;.getA:()Lcom/facebook/A;");
  auto get_b =
      DexMethod::make_method("Lcom/facebook/A;.getB:()Lcom/facebook/B;");
  auto field_c = reinterpret_cast<DexField*>(
      DexField::make_field("Lcom/facebook/A;.C:Ljava/lang/String;"));
  field_c->make_concrete(DexAccessFlags::ACC_FINAL |
                         DexAccessFlags::ACC_PUBLIC);
  AccessPath f0{AccessPathKind::FinalField, 0, field_c, {}};
  {
    AccessPath p{AccessPathKind::Parameter, 0};
    EXPECT_EQ(p, p0);
    EXPECT_EQ(hash_value(p), hash_value(p0));
  }
  {
    AccessPath p{AccessPathKind::Parameter, 0, {get_a}};
    EXPECT_NE(p, p0);
  }
  {
    AccessPath f{AccessPathKind::FinalField, 0, field_c, {}};
    EXPECT_EQ(f, f0);
    EXPECT_EQ(hash_value(f), hash_value(f0));
    AccessPath f2{AccessPathKind::FinalField, 2, field_c, {}};
    EXPECT_NE(f0, f2);
    AccessPath f_a{AccessPathKind::FinalField, 0, field_c, {get_a}};
    AccessPath f_b{AccessPathKind::FinalField, 0, field_c, {get_b}};
    EXPECT_NE(f_a, f_b);
  }
}
