/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

TEST_F(LocalPointersTest, domainOperations) {
  ptrs::Environment env1;
  ptrs::Environment env2;
  auto insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LFoo;"));
  auto insn2 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBar;"));
  auto insn3 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBaz;"));

  env1.set_fresh_pointer(0, insn1);
  env2.set_fresh_pointer(0, insn1);
  env2.set_may_escape(0, nullptr);

  env1.set_fresh_pointer(1, insn1);
  env2.set_fresh_pointer(1, insn2);

  auto joined_env = env1.join(env2);

  EXPECT_EQ(joined_env.get_pointers(0).size(), 1);
  EXPECT_EQ(*joined_env.get_pointers(0).elements().begin(), insn1);
  EXPECT_EQ(joined_env.get_pointers(1).size(), 2);
  EXPECT_THAT(joined_env.get_pointers(1).elements(),
              UnorderedElementsAre(insn1, insn2));
  EXPECT_TRUE(joined_env.may_have_escaped(insn1));
  EXPECT_FALSE(joined_env.may_have_escaped(insn2));
  EXPECT_FALSE(joined_env.may_have_escaped(insn3));
}

ptrs::InvokeToSummaryMap mark_all_invokes_as_non_escaping(const IRCode& code) {
  ptrs::InvokeToSummaryMap invoke_to_summary_map;
  for (const auto& mie : InstructionIterable(code)) {
    if (opcode::is_an_invoke(mie.insn->opcode())) {
      invoke_to_summary_map.emplace(mie.insn, ptrs::EscapeSummary({}));
    }
  }
  return invoke_to_summary_map;
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

  auto invoke_to_summary_map = mark_all_invokes_as_non_escaping(*code);
  ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_env.get_pointers(0).size(), 2);
  EXPECT_THAT(exit_env.get_pointers(0).elements(),
              UnorderedElementsAre(
                  Pointee(Eq(*(IRInstruction(OPCODE_NEW_INSTANCE)
                                   .set_type(DexType::get_type("LFoo;"))))),
                  Pointee(Eq(*(
                      IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0))))));
  auto pointers = exit_env.get_pointers(0);
  for (auto insn : pointers.elements()) {
    if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT ||
        insn->opcode() == OPCODE_NEW_INSTANCE) {
      EXPECT_FALSE(exit_env.may_have_escaped(insn));
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

  auto invoke_to_summary_map = mark_all_invokes_as_non_escaping(*code);
  ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  auto returned_ptrs = exit_env.get_pointers(0);
  EXPECT_EQ(returned_ptrs.size(), 2);
  EXPECT_THAT(
      returned_ptrs.elements(),
      UnorderedElementsAre(
          Pointee(*(IRInstruction(OPCODE_NEW_INSTANCE)
                        .set_type(DexType::get_type("LFoo;")))),
          Pointee(*(IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0)))));
  for (auto insn : returned_ptrs.elements()) {
    EXPECT_TRUE(exit_env.may_have_escaped(insn));
  }
}

TEST_F(LocalPointersTest, filledNewArrayEscape) {
  auto code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")
     (filled-new-array (v0) "[LFoo;")
     (move-result-pseudo-object v1)
     (return-object v1)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  auto invoke_to_summary_map = mark_all_invokes_as_non_escaping(*code);
  ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  auto foo_ptr_set = exit_env.get_pointers(0);
  EXPECT_EQ(foo_ptr_set.size(), 1);
  auto foo_ptr = *foo_ptr_set.elements().begin();
  EXPECT_EQ(*foo_ptr,
            *(IRInstruction(OPCODE_NEW_INSTANCE)
                  .set_type(DexType::get_type("LFoo;"))));
  EXPECT_TRUE(exit_env.may_have_escaped(foo_ptr));
}

TEST_F(LocalPointersTest, generateEscapeSummary) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (sput-object v1 "LFoo;.bar:LFoo;")
     (return-object v0)
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

TEST_F(LocalPointersTest, collectExitingPointersWithThrow) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (throw v0)
    )
  )");

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  ptrs::FixpointIterator fp_iter(cfg);
  fp_iter.run(ptrs::Environment());

  ptrs::PointerSet returned_ptrs;
  ptrs::PointerSet thrown_ptrs;
  ptrs::collect_exiting_pointers(fp_iter, *code, &returned_ptrs, &thrown_ptrs);
  EXPECT_EQ(returned_ptrs, ptrs::PointerSet::bottom());
  EXPECT_THAT(thrown_ptrs.elements(),
              UnorderedElementsAre(Pointee(
                  *(IRInstruction(IOPCODE_LOAD_PARAM_OBJECT)).set_dest(0))));

  auto summary = ptrs::get_escape_summary(fp_iter, *code);
  EXPECT_EQ(summary.returned_parameters, ptrs::ParamSet::bottom());
  EXPECT_THAT(summary.escaping_parameters, UnorderedElementsAre(0));
}

/*
 * Given the following code snippet:
 *
 *   class Foo {
 *      public static Foo newInstance() {
 *        Foo instance = new Foo();
 *        instance.setValue(123);
 *        return instance;
 *      }
 *   }
 *
 * We wish the callers of newInstance() to be able to treat its return value as
 * non-escaping.
 */
TEST_F(LocalPointersTest, returnFreshValue) {
  ptrs::EscapeSummary fresh_return_summary;
  // First, check that we generate the right summary from the callee.
  {
    auto code = assembler::ircode_from_string(R"(
      (
       (new-instance "LFoo;")
       (move-result-pseudo-object v0)
       (invoke-direct (v0) "LFoo;.<init>:()V")
       (return v0)
      )
    )");

    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();

    auto invoke_to_summary_map = mark_all_invokes_as_non_escaping(*code);
    ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
    fp_iter.run(ptrs::Environment());

    fresh_return_summary = ptrs::get_escape_summary(fp_iter, *code);
    EXPECT_EQ(fresh_return_summary.returned_parameters,
              ptrs::ParamSet(ptrs::FRESH_RETURN));
    EXPECT_THAT(fresh_return_summary.escaping_parameters,
                UnorderedElementsAre());
  }

  // Now check that the caller handles the summary correctly.
  {
    auto code = assembler::ircode_from_string(R"(
      (
       (invoke-static () "LFoo;.newInstance:()LFoo;")
       (move-result-object v0)
       (return v0)
      )
    )");

    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();

    ptrs::InvokeToSummaryMap invoke_to_summary_map;
    const IRInstruction* invoke_insn{nullptr};
    for (const auto& mie : InstructionIterable(code.get())) {
      auto insn = mie.insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        invoke_insn = insn;
        invoke_to_summary_map.emplace(insn, fresh_return_summary);
      }
    }

    ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
    fp_iter.run(ptrs::Environment());

    auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
    EXPECT_THAT(exit_env.get_pointers(0).elements(),
                UnorderedElementsAre(Pointee(Eq(*invoke_insn))));
    EXPECT_FALSE(exit_env.may_have_escaped(invoke_insn));
  }
}

/*
 * Check that we correctly analyze the cases where we return a newly-allocated
 * value that has escaped.
 */
TEST_F(LocalPointersTest, returnEscapedValue) {
  ptrs::EscapeSummary fresh_return_summary;
  // First, check that we generate the right summary from the callee.
  {
    auto code = assembler::ircode_from_string(R"(
      (
       (new-instance "LFoo;")
       (move-result-pseudo-object v0)
       (invoke-direct (v0) "LFoo;.<init>:()V")
       (sput-object v0 "LFoo;.a:LFoo;") ; v0 escapes here
       (return v0)
      )
    )");

    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();

    auto invoke_to_summary_map = mark_all_invokes_as_non_escaping(*code);
    ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
    fp_iter.run(ptrs::Environment());

    fresh_return_summary = ptrs::get_escape_summary(fp_iter, *code);
    EXPECT_EQ(fresh_return_summary.returned_parameters, ptrs::ParamSet::top());
    EXPECT_THAT(fresh_return_summary.escaping_parameters,
                UnorderedElementsAre());
  }

  // Now check that the caller handles the summary correctly.
  {
    auto code = assembler::ircode_from_string(R"(
      (
       (invoke-static () "LFoo;.newEscapedInstance:()LFoo;")
       (move-result-object v0)
       (return v0)
      )
    )");

    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();

    ptrs::InvokeToSummaryMap invoke_to_summary_map;
    const IRInstruction* invoke_insn{nullptr};
    for (const auto& mie : InstructionIterable(code.get())) {
      auto insn = mie.insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        invoke_insn = insn;
        invoke_to_summary_map.emplace(insn, fresh_return_summary);
      }
    }

    ptrs::FixpointIterator fp_iter(cfg, invoke_to_summary_map);
    fp_iter.run(ptrs::Environment());

    auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
    EXPECT_THAT(exit_env.get_pointers(0).elements(),
                UnorderedElementsAre(Pointee(Eq(*invoke_insn))));
    EXPECT_TRUE(exit_env.may_have_escaped(invoke_insn));
  }
}
