/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalPointersAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"

namespace ptrs = local_pointers;

using namespace testing;

class LocalPointersTest : public RedexTest {};

std::unordered_set<const IRInstruction*> to_set(const ptrs::PointerSet& pset) {
  if (!pset.is_value()) {
    return std::unordered_set<const IRInstruction*>{};
  }
  const auto& elems = pset.elements();
  std::unordered_set<const IRInstruction*> result(elems.begin(), elems.end());
  return result;
}

TEST_F(LocalPointersTest, domainOperations) {
  ptrs::Environment env1;
  ptrs::Environment env2;
  auto insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LFoo;"));
  auto insn2 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBar;"));
  auto insn3 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBaz;"));

  env1.set_pointer_at(0, insn1, EscapeState::NOT_ESCAPED);
  env2.set_pointer_at(0, insn1, EscapeState::MAY_ESCAPE);

  env1.set_pointer_at(1, insn1, EscapeState::NOT_ESCAPED);
  env2.set_pointer_at(1, insn2, EscapeState::NOT_ESCAPED);

  auto joined_env = env1.join(env2);

  EXPECT_EQ(joined_env.get_pointers(0).size(), 1);
  EXPECT_EQ(*joined_env.get_pointers(0).elements().begin(), insn1);
  EXPECT_EQ(joined_env.get_pointers(1).size(), 2);
  EXPECT_THAT(to_set(joined_env.get_pointers(1)),
              UnorderedElementsAre(insn1, insn2));
  EXPECT_EQ(joined_env.get_pointee(insn1),
            EscapeDomain(EscapeState::MAY_ESCAPE));
  EXPECT_EQ(joined_env.get_pointee(insn2),
            EscapeDomain(EscapeState::NOT_ESCAPED));
  EXPECT_EQ(joined_env.get_pointee(insn3), EscapeDomain(EscapeState::BOTTOM));
}

TEST_F(LocalPointersTest, simple) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (if-nez v0 :true)
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")
     (:true)
     (return-void)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ptrs::InvokeToSummaryMap invoke_to_summary_map;
  for (const auto& mie : InstructionIterable(code.get())) {
    if (is_invoke(mie.insn->opcode())) {
      invoke_to_summary_map.emplace(mie.insn, ptrs::EscapeSummary({}));
    }
  }
  ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_env.get_pointers(0).size(), 2);
  EXPECT_THAT(to_set(exit_env.get_pointers(0)),
              UnorderedElementsAre(
                  Pointee(Eq(*(IRInstruction(OPCODE_NEW_INSTANCE)
                                   .set_type(DexType::get_type("LFoo;"))))),
                  Pointee(Eq(*(
                      IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0))))));
  for (auto insn : exit_env.get_pointers(0).elements()) {
    if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
      EXPECT_EQ(exit_env.get_pointee(insn),
                EscapeDomain(EscapeState::ONLY_PARAMETER_DEPENDENT));
    } else if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      EXPECT_EQ(exit_env.get_pointee(insn),
                EscapeDomain(EscapeState::NOT_ESCAPED));
    }
  }
}

TEST_F(LocalPointersTest, aliasEscape) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (if-nez v0 :true)
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")
     (:true)
     (move-object v1 v0)
     (sput-object v1 "LFoo;.bar:LFoo;")
     (return v0)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ptrs::InvokeToSummaryMap invoke_to_summary_map;
  for (const auto& mie : InstructionIterable(code.get())) {
    if (is_invoke(mie.insn->opcode())) {
      invoke_to_summary_map.emplace(mie.insn, ptrs::EscapeSummary({}));
    }
  }
  ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  auto returned_ptrs = exit_env.get_pointers(0);
  EXPECT_EQ(returned_ptrs.size(), 2);
  EXPECT_THAT(
      to_set(returned_ptrs),
      UnorderedElementsAre(
          Pointee(*(IRInstruction(OPCODE_NEW_INSTANCE)
                        .set_type(DexType::get_type("LFoo;")))),
          Pointee(*(IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0)))));
  for (auto insn : returned_ptrs.elements()) {
    EXPECT_EQ(exit_env.get_pointee(insn),
              EscapeDomain(EscapeState::MAY_ESCAPE));
  }
}

TEST_F(LocalPointersTest, generateEscapeSummary) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (sput-object v1 "LFoo;.bar:LFoo;")
     (return v0)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  ptrs::FixpointIterator fp_iter(cfg);
  fp_iter.run(ptrs::Environment());

  auto summary = ptrs::get_escape_summary(fp_iter, *code);
  EXPECT_EQ(summary.returned_parameters, ptrs::ParamSet({0}));
  EXPECT_THAT(summary.escaping_parameters, UnorderedElementsAre(1));

  // Test (de)serialization.
  std::stringstream ss;
  ss << to_s_expr(summary);
  EXPECT_EQ(ss.str(), "((#1) (#0))");

  sparta::s_expr_istream s_expr_in(ss);
  sparta::s_expr summary_s_expr;
  s_expr_in >> summary_s_expr;
  auto summary_copy = ptrs::EscapeSummary::from_s_expr(summary_s_expr);
  EXPECT_EQ(summary_copy.returned_parameters, ptrs::ParamSet({0}));
  EXPECT_THAT(summary_copy.escaping_parameters, UnorderedElementsAre(1));
}

TEST_F(LocalPointersTest, generateEscapeSummary2) {
  auto code = assembler::ircode_from_string(R"(
    (
     (sget-object "LFoo;.bar:LFoo;")
     (move-result-pseudo-object v0)
     (return v0)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  ptrs::FixpointIterator fp_iter(cfg);
  fp_iter.run(ptrs::Environment());

  auto summary = ptrs::get_escape_summary(fp_iter, *code);
  EXPECT_EQ(summary.returned_parameters, ptrs::ParamSet::top());
  EXPECT_THAT(summary.escaping_parameters, UnorderedElementsAre());

  // Test (de)serialization.
  std::stringstream ss;
  ss << to_s_expr(summary);
  EXPECT_EQ(ss.str(), "(() Top)");

  sparta::s_expr_istream s_expr_in(ss);
  sparta::s_expr summary_s_expr;
  s_expr_in >> summary_s_expr;
  auto summary_copy = ptrs::EscapeSummary::from_s_expr(summary_s_expr);
  EXPECT_EQ(summary_copy.returned_parameters, ptrs::ParamSet::top());
  EXPECT_THAT(summary_copy.escaping_parameters, UnorderedElementsAre());
}
