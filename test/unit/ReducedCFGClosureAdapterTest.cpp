/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "Debug.h"
#include "IRAssembler.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "OutlinerTypeAnalysis.h"
#include "RedexTest.h"
#include "ReducedCFGClosureAdapter.h"
#include "ReducedControlFlow.h"

using method_splitting_impl::ReducedBlock;
using method_splitting_impl::ReducedControlFlowGraph;

class ReducedCFGClosureAdapterTest : public RedexTest {};

namespace {

IRInstruction* find_invoke_of(DexMethod* method, std::string_view name) {
  for (const auto& mie : InstructionIterable(method->get_code()->cfg())) {
    if (mie.insn->has_method() && mie.insn->get_method()->str() == name) {
      return mie.insn;
    }
  }
  not_reached();
}

cfg::Block* block_of(cfg::ControlFlowGraph& cfg, const IRInstruction* insn) {
  for (auto* block : cfg.blocks()) {
    for (const auto& mie : InstructionIterable(block)) {
      if (mie.insn == insn) {
        return block;
      }
    }
  }
  not_reached();
}

// `LB;` extends `LA;`. `foo` receives an `LA;` and, inside a loop, both uses it
// as an `LA;` and overwrites it with an `LB;`. The overwrite is a def that sits
// INSIDE the loop body and still reaches the loop header, along the back edge.
// That is the shape the guard exists for; a closure rooted at the header has a
// cover containing one of its own reaching defs.
DexMethod* create_back_edge_method() {
  ClassCreator object_creator(type::java_lang_Object());
  object_creator.create();
  ClassCreator a_creator(DexType::make_type("LA;"));
  a_creator.set_super(type::java_lang_Object());
  auto* a_type = a_creator.create()->get_type();
  ClassCreator b_creator(DexType::make_type("LB;"));
  b_creator.set_super(a_type);
  b_creator.create();

  auto* foo_method = assembler::method_from_string(R"(
      (method (public static) "LFoo;.foo:(LA;Z)V" (
        (load-param-object v0)
        (load-param v1)
        (:loop)
        (invoke-static (v0) "LFoo;.inspect:(LA;)V")
        (invoke-static () "LFoo;.makeB:()LB;")
        (move-result-object v0)
        (invoke-static (v0) "LFoo;.useB:(LB;)V")
        (if-nez v1 :loop)
        (return-void)
      )))");
  foo_method->get_code()->build_cfg();
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());
  foo_creator.add_method(foo_method);
  foo_creator.create();
  return foo_method;
}

} // namespace

// A split's parameter type is the MEET of its live-in's use demands, so it must
// never fall below the type the caller actually supplies. Seeding the walk from
// every reaching def breaks that: a def produced inside the cover contributes
// its own uses' demands, and here `useB` demands `LB;` -- a strict subtype of
// the `LA;` the caller passes. Both halves are asserted, so the test fails if
// the guard stops discriminating in either direction.
TEST_F(ReducedCFGClosureAdapterTest, in_cover_seed_defs_do_not_narrow_live_in) {
  auto* foo_method = create_back_edge_method();
  auto& cfg = foo_method->get_code()->cfg();
  outliner_impl::OutlinerTypeAnalysis ota(foo_method);
  ReducedControlFlowGraph rcfg(cfg);

  Lazy<UnorderedMap<IRInstruction*, const ReducedBlock*>> insns([&rcfg] {
    auto res =
        std::make_unique<UnorderedMap<IRInstruction*, const ReducedBlock*>>();
    for (const auto* reduced_block : rcfg.blocks()) {
      for (const auto* block : UnorderedIterable(reduced_block->blocks)) {
        for (const auto& mie : InstructionIterable(block)) {
          res->emplace(mie.insn, reduced_block);
        }
      }
    }
    return res;
  });
  Lazy<live_range::DefUseChains> def_uses([&cfg] {
    return std::make_unique<live_range::DefUseChains>(
        live_range::Chains(cfg).get_def_use_chains());
  });

  auto* first_insn = find_invoke_of(foo_method, "inspect");
  auto reg = first_insn->src(0);
  // The loop is a strongly-connected component, so it collapses to a single
  // reduced block, and that block alone is the closure's cover.
  UnorderedSet<const ReducedBlock*> cover{
      rcfg.get_reduced_block(block_of(cfg, first_insn))};

  const auto* a_type = DexType::get_type("LA;");
  const auto* b_type = DexType::get_type("LB;");
  ASSERT_NE(a_type, nullptr);
  ASSERT_NE(b_type, nullptr);

  {
    // Seeding from every reaching def: `move-result-object` lies inside the
    // cover, so `useB`'s `LB;` demand joins the meet and wins. The split would
    // be typed against a subtype its caller never passes, and the host fails
    // verification handing it an `LA;`.
    outliner_impl::ReducedCFGClosureAdapter ca(
        ota, first_insn, insns, cover, def_uses,
        /* exclude_in_cover_seed_defs */ false);
    EXPECT_EQ(ota.get_type_demand(ca, reg), b_type);
  }

  {
    // With in-cover seed defs excluded, only the caller's `load-param-object`
    // seeds the walk, and the meet stays at the incoming `LA;`.
    outliner_impl::ReducedCFGClosureAdapter ca(
        ota, first_insn, insns, cover, def_uses,
        /* exclude_in_cover_seed_defs */ true);
    EXPECT_EQ(ota.get_type_demand(ca, reg), a_type);
  }
}
