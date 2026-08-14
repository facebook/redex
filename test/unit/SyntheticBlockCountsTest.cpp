/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SyntheticBlockCounts.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "MethodProfiles.h"
#include "RedexContext.h"
#include "RedexTest.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "TypeUtil.h"

// Fixture is a friend of the pass (see SyntheticBlockCounts.h) so it can drive
// the per-method core and read the private accumulator directly.
class SyntheticBlockCountsTest : public RedexTest {
 public:
  void SetUp() override { g_redex->set_sb_interaction_index({{"Fake", 0}}); }

  DexMethod* make(const std::string& code) {
    size_t c = s_counter.fetch_add(1);
    std::string name = "LD" + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name)};
    cc.set_super(type::java_lang_Object());
    auto* m = DexMethod::make_method(name + ".f:()V")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                  assembler::ircode_from_string(code),
                                  /*is_virtual=*/false);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);
    cc.create();
    m->get_code()->build_cfg();
    return m;
  }

  // Like make() but with an explicit class name, so the body can self-reference
  // (e.g. a recursive invoke of its own LREC;.f:()V).
  DexMethod* make_named(const std::string& cls, const std::string& body) {
    ClassCreator cc{DexType::make_type(cls)};
    cc.set_super(type::java_lang_Object());
    auto* m = DexMethod::make_method(cls + ".f:()V")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                  assembler::ircode_from_string(body),
                                  /*is_virtual=*/false);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);
    cc.create();
    m->get_code()->build_cfg();
    return m;
  }

  SyntheticBlockCountsPass::MethodResult run(
      DexMethod* m,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.process_method(m, m->get_code()->cfg(), profiles, inv_slot);
  }

  // Build a Scope (distinct classes) from a set of methods.
  static Scope scope_of(const std::vector<DexMethod*>& ms) {
    Scope scope;
    UnorderedSet<DexClass*> seen;
    for (auto* m : ms) {
      auto* cls = type_class(m->get_class());
      if (cls != nullptr && seen.count(cls) == 0) {
        seen.insert(cls);
        scope.push_back(cls);
      }
    }
    return scope;
  }

  // A caller (make()d as LD<n>) whose body invokes `callee` once on its entry
  // path, plus a covered entry source block.
  DexMethod* make_caller(DexMethod* callee) {
    std::string code =
        "(\n"
        "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
        "  (invoke-static () \"" +
        show(callee) +
        "\")\n"
        "  (return-void)\n"
        ")";
    auto* caller = make(code);
    // The call graph is root-seeded BFS; mark the caller a root so its
    // callsites (the invoke of `callee`) are explored and the edge is built.
    caller->rstate.set_root();
    return caller;
  }

  // A concrete virtual method `g:()V` in class `cls_name` extending `super`.
  DexMethod* make_vmethod(const std::string& cls_name,
                          DexType* super,
                          const std::string& body) {
    ClassCreator cc{DexType::make_type(cls_name)};
    cc.set_super(super);
    auto* m = DexMethod::make_method(cls_name + ".g:()V")
                  ->make_concrete(ACC_PUBLIC,
                                  assembler::ircode_from_string(body),
                                  /*is_virtual=*/true);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);
    cc.create();
    m->get_code()->build_cfg();
    return m;
  }

  SyntheticBlockCountsPass::InterStats run_inter(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.run_intermethod_core(scope, profiles, inv_slot);
  }

  // Fixture is the friend; TEST_F bodies (a derived class) are not, so private
  // members are reached through helpers like this.
  std::vector<int64_t> missing_hit(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.count_missing_hit_methods(scope, profiles, inv_slot);
  }

  SyntheticBlockCountsPass::ClampStats clamp(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.clamp_post_pass_core(scope, profiles, inv_slot);
  }

  SyntheticBlockCountsPass::ReflowStats reflow(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.reflow_post_pass_core(scope, profiles, inv_slot);
  }

  // Non-ghost successors of a block, as a value list.
  static std::vector<float> arm_vals(cfg::Block* b) {
    std::vector<float> out;
    for (auto* e : b->succs()) {
      if (e->type() != cfg::EDGE_GHOST) {
        out.push_back(fv(e->target()));
      }
    }
    return out;
  }

  static method_profiles::MethodProfiles profile_with(DexMethod* m,
                                                      double call_count) {
    UnorderedMap<const DexMethodRef*, method_profiles::Stats> data;
    data.emplace(m,
                 method_profiles::Stats{/*appear_percent=*/100.0,
                                        /*call_count=*/call_count,
                                        /*order_percent=*/0.0,
                                        /*min_api_level=*/0});
    return method_profiles::MethodProfiles::initialize("Fake", std::move(data));
  }

  static method_profiles::MethodProfiles profiles_of(
      const std::vector<std::pair<DexMethod*, double>>& entries) {
    UnorderedMap<const DexMethodRef*, method_profiles::Stats> data;
    for (const auto& e : entries) {
      data.emplace(e.first,
                   method_profiles::Stats{/*appear_percent=*/100.0,
                                          /*call_count=*/e.second,
                                          /*order_percent=*/0.0,
                                          /*min_api_level=*/0});
    }
    return method_profiles::MethodProfiles::initialize("Fake", std::move(data));
  }

  // First-SB val of a block (-1 if none). Blocks are identified structurally
  // rather than by SB id, since the assembler's numeric id token is not
  // necessarily preserved as SourceBlock::id.
  static float fv(cfg::Block* b, size_t slot = 0) {
    auto* sb = source_blocks::get_first_source_block(b);
    return sb != nullptr ? sb->get_val(slot).value_or(-1.0f) : -1.0f;
  }

  static cfg::Block* single_succ(cfg::Block* b) {
    cfg::Block* out = nullptr;
    for (auto* e : b->succs()) {
      if (e->type() != cfg::EDGE_GHOST) {
        out = e->target();
      }
    }
    return out;
  }

  // Ordered list of every SourceBlock val (walking chains), block order.
  static std::vector<float> all_vals(cfg::ControlFlowGraph& cfg,
                                     size_t slot = 0) {
    std::vector<float> out;
    for (auto* b : cfg.blocks()) {
      source_blocks::foreach_source_block(b, [&](SourceBlock* sb) {
        auto v = sb->get_val(slot);
        if (v) {
          out.push_back(*v);
        }
      });
    }
    return out;
  }

  static std::vector<float> all_appear(cfg::ControlFlowGraph& cfg,
                                       size_t slot = 0) {
    std::vector<float> out;
    for (auto* b : cfg.blocks()) {
      source_blocks::foreach_source_block(b, [&](SourceBlock* sb) {
        auto a = sb->get_appear100(slot);
        if (a) {
          out.push_back(*a);
        }
      });
    }
    return out;
  }

  SyntheticBlockCountsPass m_pass;
  static std::atomic<size_t> s_counter;
};

std::atomic<size_t> SyntheticBlockCountsTest::s_counter{0};

// A covered diamond: entry splits two ways and the arms re-merge. Flow splits
// at the branch and conserves at the merge.
TEST_F(SyntheticBlockCountsTest, DiamondExactSplitMerge) {
  // Each arm carries a real instruction so build_cfg keeps it as a distinct
  // block (a block holding only a source block gets folded away).
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :right)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (const v1 1)
      (goto :merge)
      (:right)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (const v1 2)
      (goto :merge)
      (:merge)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto& cfg = m->get_code()->cfg();
  auto* entry = cfg.entry_block();
  EXPECT_FLOAT_EQ(fv(entry), 100.0f); // entry anchored to call_count
  double arm_sum = 0.0;
  cfg::Block* merge = nullptr;
  for (auto* e : entry->succs()) {
    if (e->type() == cfg::EDGE_GHOST) {
      continue;
    }
    auto* arm = e->target();
    arm_sum += fv(arm);
    merge = single_succ(arm); // both arms converge on the merge block
  }
  EXPECT_FLOAT_EQ((float)arm_sum, 100.0f); // split conserves
  ASSERT_NE(merge, nullptr);
  EXPECT_FLOAT_EQ(fv(merge), 100.0f); // merge conserves
}

// A self-loop: the header is amplified by the geometric loop multiplier
// (p_back=0.5 -> x2), the exit conserves back to the entry count, and the
// header is the only block allowed to exceed the entry count.
TEST_F(SyntheticBlockCountsTest, SelfLoopGeometric) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:head)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :head)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto& cfg = m->get_code()->cfg();
  auto* entry = cfg.entry_block();
  auto* head = single_succ(entry);
  ASSERT_NE(head, nullptr);
  cfg::Block* exit = nullptr;
  for (auto* e : head->succs()) {
    if (e->type() != cfg::EDGE_GHOST && e->target() != head) {
      exit = e->target();
    }
  }
  ASSERT_NE(exit, nullptr);
  EXPECT_FLOAT_EQ(fv(entry), 100.0f); // entry anchored
  EXPECT_FLOAT_EQ(fv(head), 200.0f); // header = C * 1/(1-0.5)
  EXPECT_FLOAT_EQ(fv(exit), 100.0f); // exit conserves back to C
  EXPECT_GT(fv(head), fv(entry)); // header is the only >C block
  EXPECT_LE(fv(exit), fv(entry));
}

// A header/body/latch loop whose EXIT is at the header (the `while (cond)`
// shape): header checks the condition (50% exit, 50% into body), body branches
// back unconditionally. The cyclic probability is
//   p_back = freq_local(latch) * edge_prob(latch->header) = 0.5 * 1.0 = 0.5,
// so the header multiplier is 1/(1-0.5) = 2 -- NOT the x10 iteration cap that a
// naive `p_back = edge_prob(latch->header) = 1.0` would produce. The self-loop
// test above hides that bug because header == latch (freq_local(latch) == 1).
TEST_F(SyntheticBlockCountsTest, LoopHeaderExitCorrectMultiplier) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:head)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :exit)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (const v1 1)
      (goto :head)
      (:exit)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto& cfg = m->get_code()->cfg();
  auto* head = single_succ(cfg.entry_block());
  ASSERT_NE(head, nullptr);
  EXPECT_FLOAT_EQ(fv(head), 200.0f); // 100 * 1/(1-0.5); NOT the ~1000 (x10) bug
}

// A covered loop header whose back-edge was never measured: the body/latch is
// cold (val 0) for this interaction, so the header->body edge is
// coverage-pinned to 0 and freq_local(latch) collapses to 0, giving p_back ==
// 0. Because the header itself is still covered_hot, loop_geometric_mult falls
// back to `default_backedge_prob` (0.9) -> 1/(1-0.9) == 10 (the iteration cap),
// NOT the x2 a measured 0.5 back-edge would give. This fallback is the only
// structural source of a non-x2 trip count.
TEST_F(SyntheticBlockCountsTest,
       DefaultBackedgeProbFallbackWhenBackEdgeUnmeasured) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:head)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :exit)
      (.src_block "LD;.f:()V" 2 (0.0 1.0))
      (const v1 1)
      (goto :head)
      (:exit)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto& cfg = m->get_code()->cfg();
  auto* head = single_succ(cfg.entry_block());
  ASSERT_NE(head, nullptr);
  // 100 * 1/(1-0.9) ~= 1000 (fallback), NOT 200 (a measured 0.5 back-edge).
  EXPECT_NEAR(fv(head), 1000.0f, 1.0f);
}

// Nested loops: an inner loop inside an outer loop, each a 2-way header with a
// measured 0.5 back-edge (-> x2). The inner multiplier must compound into the
// outer solve: as the outer's local frequency propagation reaches the inner
// header it multiplies by the inner multiplier (`in *= loop_mult[inner]`), so
// the outer back-edge carries the inner's amplified iteration count. Without
// that compounding the outer header would be ~133 (p_back 0.25), not 200.
TEST_F(SyntheticBlockCountsTest, NestedLoopsCompoundMultiplier) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:outer)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :oexit)
      (:inner)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (if-eqz v0 :olatch)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (const v1 1)
      (goto :inner)
      (:olatch)
      (.src_block "LD;.f:()V" 4 (1.0 1.0))
      (goto :outer)
      (:oexit)
      (.src_block "LD;.f:()V" 5 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto& cfg = m->get_code()->cfg();
  auto* outer = single_succ(cfg.entry_block());
  ASSERT_NE(outer, nullptr);
  // The inner header is the outer successor that itself branches (>=2 real
  // succs); the outer exit only returns.
  cfg::Block* inner = nullptr;
  for (auto* e : outer->succs()) {
    if (e->type() == cfg::EDGE_GHOST) {
      continue;
    }
    size_t nsucc = 0;
    for (auto* te : e->target()->succs()) {
      if (te->type() != cfg::EDGE_GHOST) {
        nsucc++;
      }
    }
    if (nsucc >= 2) {
      inner = e->target();
    }
  }
  ASSERT_NE(inner, nullptr);
  // Both headers reach 100 * 2 == 200. The outer 200 is the compounding signal:
  // it needs the inner multiplier folded into the outer back-edge frequency.
  EXPECT_FLOAT_EQ(fv(outer), 200.0f);
  EXPECT_FLOAT_EQ(fv(inner), 200.0f);
}

// Coverage support is pinned: a block that is cold (val==0) stays 0, and every
// covered block stays strictly > 0.
TEST_F(SyntheticBlockCountsTest, SupportPreservation) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :cold)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (const v1 1)
      (goto :end)
      (:cold)
      (.src_block "LD;.f:()V" 2 (0.0 0.0))
      (const v1 2)
      (goto :end)
      (:end)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto vals = all_vals(m->get_code()->cfg());
  size_t cold = 0, hot = 0;
  for (float v : vals) {
    if (v == 0.0f) {
      cold++;
    } else {
      EXPECT_GT(v, 0.0f);
      hot++;
    }
  }
  EXPECT_EQ(cold, 1u); // exactly the one pinned-cold block stays 0
  EXPECT_GT(hot, 0u);
}

// appear100 is never modified.
TEST_F(SyntheticBlockCountsTest, Appear100BytePreservation) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 12.5))
      (const v0 0)
      (if-eqz v0 :right)
      (.src_block "LD;.f:()V" 1 (1.0 33.0))
      (goto :merge)
      (:right)
      (.src_block "LD;.f:()V" 2 (1.0 47.5))
      (:merge)
      (.src_block "LD;.f:()V" 3 (1.0 88.0))
      (return-void)
    ))");
  auto before = all_appear(m->get_code()->cfg());
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto after = all_appear(m->get_code()->cfg());
  EXPECT_EQ(before, after);
}

// A method with no profile for the interaction is left untouched for that slot.
TEST_F(SyntheticBlockCountsTest, NoProfileSkipsSlot) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :right)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (goto :merge)
      (:right)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (:merge)
      (.src_block "LD;.f:()V" 3 (1.0 1.0))
      (return-void)
    ))");
  method_profiles::MethodProfiles empty; // no stats for m
  auto before = all_vals(m->get_code()->cfg());
  auto res = run(m, empty);
  auto after = all_vals(m->get_code()->cfg());
  EXPECT_EQ(before, after); // vals untouched
  EXPECT_EQ(res.solve_fallback_no_profile, 1u);
  EXPECT_EQ(res.methods_with_usable_profile, 0u);
}

// A negative call_count is garbage, not a usable anchor: the method is left
// as-is (boolean vals preserved) rather than collapsing every covered block to
// epsilon from a negative-scaled solve.
TEST_F(SyntheticBlockCountsTest, NegativeCallCountLeavesBooleanValsUntouched) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto profiles = profile_with(m, -5.0);
  auto before = all_vals(m->get_code()->cfg());
  auto res = run(m, profiles);
  auto after = all_vals(m->get_code()->cfg());
  EXPECT_EQ(before, after); // anchor rejected: original 1.0 kept, not epsilon
  EXPECT_EQ(res.solve_fallback_no_profile, 1u);
  EXPECT_EQ(res.methods_with_usable_profile, 0u);
}

// Re-running the solve produces identical values (deterministic / idempotent).
TEST_F(SyntheticBlockCountsTest, DeterministicIdempotent) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:head)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :head)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles);
  auto first = all_vals(m->get_code()->cfg());
  run(m, profiles);
  auto second = all_vals(m->get_code()->cfg());
  EXPECT_EQ(first, second);
}

// A covered method with no usable call_count anchor -- an absent profile row,
// or a non-finite / negative call_count -- is counted. A properly-profiled
// method is not; and, consistent with `usable_call_count`, neither is a method
// whose call_count is exactly 0 (a usable, genuinely-cold anchor, not a missing
// hit).
TEST_F(SyntheticBlockCountsTest, MissingHitMethodsCountsCoveredUnprofiled) {
  const std::string body = R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))";
  auto* profiled = make(body); // covered + call_count 100 -> NOT counted
  auto* unprofiled = make(body); // covered + no row       -> counted
  auto* zero_count =
      make(body); // covered + call_count 0  -> usable, NOT counted
  auto profiles = profiles_of({{profiled, 100.0}, {zero_count, 0.0}});
  auto scope = scope_of({profiled, unprofiled, zero_count});

  auto counts = missing_hit(scope, profiles);
  ASSERT_EQ(counts.size(), 1u);
  EXPECT_EQ(counts[0], 1); // only the unprofiled method
}

// Inter-method: a profiled caller's call_count forward-fills an UNPROFILED
// callee. The callee's entry, boolean before, is heated to the caller's count.
TEST_F(SyntheticBlockCountsTest, InterMethodForwardFillsUnprofiledCallee) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profile_with(caller, 100.0); // callee is unprofiled
  auto scope = scope_of({caller, callee});

  auto st = run_inter(scope, profiles);

  auto& caller_cfg = caller->get_code()->cfg();
  auto& callee_cfg = callee->get_code()->cfg();
  // Profiled caller pinned to its own call_count (strict superset of naive).
  EXPECT_FLOAT_EQ(fv(caller_cfg.entry_block()), 100.0f);
  // Unprofiled callee forward-filled: entry heated from boolean 1.0 to ~100.
  EXPECT_GT(fv(callee_cfg.entry_block()), 1.0f);
  EXPECT_FLOAT_EQ(fv(callee_cfg.entry_block()), 100.0f);
  EXPECT_GE(st.pairs_pinned, 1u);
  EXPECT_GE(st.pairs_forward_filled, 1u);
}

// Fan-in: two profiled callers each invoke the same unprofiled callee once on
// their entry path (k == 1). The callee's scale is the SUM over its callers,
// 100 + 30 == 130. Every other forward test is a single-caller chain; this is
// the only one exercising the sum-over-callers term of the forward formula.
TEST_F(SyntheticBlockCountsTest, InterMethodSumsOverMultipleCallers) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* a = make_caller(callee);
  auto* b = make_caller(callee);
  auto profiles = profiles_of({{a, 100.0}, {b, 30.0}});
  auto scope = scope_of({a, b, callee});

  run_inter(scope, profiles);

  EXPECT_FLOAT_EQ(fv(a->get_code()->cfg().entry_block()), 100.0f); // pinned
  EXPECT_FLOAT_EQ(fv(b->get_code()->cfg().entry_block()), 30.0f); // pinned
  // Unprofiled callee = scale(a)*k + scale(b)*k = 100*1 + 30*1 == 130.
  EXPECT_FLOAT_EQ(fv(callee->get_code()->cfg().entry_block()), 130.0f);
}

// Forward k > 1: the caller invokes the helper from its loop header, which the
// per-method shape runs twice (p_back 0.5 -> x2), so k(caller->helper) == 2.
// The unprofiled helper is therefore heated ABOVE the caller's own count,
// 100 * 2 == 200 -- the "hot helper called in a loop" case, the headline
// benefit of inter-method flow, which the single-call-on-entry tests (k == 1)
// never reach.
TEST_F(SyntheticBlockCountsTest,
       InterMethodLoopCallSiteHeatsHelperAboveCaller) {
  auto* helper = make(R"((
      (.src_block "LH;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  std::string code =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (:head)\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(helper) +
      "\")\n"
      "  (if-eqz v0 :exit)\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (goto :head)\n"
      "  (:exit)\n"
      "  (.src_block \"LC;.f:()V\" 3 (1.0 1.0))\n"
      "  (return-void)\n"
      ")";
  auto* caller = make(code);
  caller->rstate.set_root();
  auto profiles = profile_with(caller, 100.0);
  auto scope = scope_of({caller, helper});

  run_inter(scope, profiles);

  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()),
                  100.0f); // pinned
  // k > 1 at the loop-header call site: the helper is hotter than its caller.
  EXPECT_GT(fv(helper->get_code()->cfg().entry_block()), 100.0f);
  EXPECT_NEAR(fv(helper->get_code()->cfg().entry_block()), 200.0f, 1.0f);
}

// Inter-method RelevanceSet prune: a method NOT reachable from any profiled
// root (here an isolated, unprofiled method) is left byte-identical -- it is
// never solved (shape_solves counts only the pinned+reachable set) and never
// written, while a reachable callee is still forward-filled. The cache-miss
// canary stays 0. This guards the coverage-gate/RelevanceSet perf rewrite (see
// intermethod-perf-plan.md).
TEST_F(SyntheticBlockCountsTest, InterMethodRelevanceSetPrunesUnreachable) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  // Isolated: covered (boolean 1.0) but called by nobody -> non-relevant.
  auto* isolated = make(R"((
      (.src_block "LY;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto profiles = profile_with(caller, 100.0);
  auto scope = scope_of({caller, callee, isolated});

  auto st = run_inter(scope, profiles);

  auto& callee_cfg = callee->get_code()->cfg();
  auto& isolated_cfg = isolated->get_code()->cfg();
  // Reachable callee still forward-filled...
  EXPECT_FLOAT_EQ(fv(callee_cfg.entry_block()), 100.0f);
  // ...but the unreachable method is byte-identical (boolean 1.0 preserved).
  EXPECT_FLOAT_EQ(fv(isolated_cfg.entry_block()), 1.0f);
  // Only the relevant set (caller + callee) is solved; isolated is pruned.
  EXPECT_EQ(st.shape_solves, 2u);
  EXPECT_EQ(st.relevance_set_size, 2u);
  EXPECT_EQ(st.shape_cache_misses, 0u); // canary
}

// Inter-method NFC + determinism on the heated (unprofiled) callee: a pinned
// cold block stays 0, appear100 is untouched, and a second run is identical.
TEST_F(SyntheticBlockCountsTest, InterMethodNfcAndDeterministic) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 5.0))
      (const v0 0)
      (if-eqz v0 :cold)
      (.src_block "LX;.f:()V" 1 (1.0 6.0))
      (const v1 1)
      (goto :end)
      (:cold)
      (.src_block "LX;.f:()V" 2 (0.0 0.0))
      (const v1 2)
      (goto :end)
      (:end)
      (.src_block "LX;.f:()V" 3 (1.0 7.0))
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profile_with(caller, 100.0);
  auto scope = scope_of({caller, callee});

  auto appear_before = all_appear(callee->get_code()->cfg());
  run_inter(scope, profiles);
  auto vals1 = all_vals(callee->get_code()->cfg());
  auto appear_after = all_appear(callee->get_code()->cfg());

  EXPECT_EQ(appear_before, appear_after); // appear100 never touched
  size_t cold = 0;
  for (float v : vals1) {
    if (v == 0.0f) {
      cold++;
    } else {
      EXPECT_GT(v, 0.0f); // every covered block stays strictly > 0
    }
  }
  EXPECT_EQ(cold, 1u); // exactly the one pinned-cold block stays 0

  run_inter(scope, profiles);
  auto vals2 = all_vals(callee->get_code()->cfg());
  EXPECT_EQ(vals1, vals2); // deterministic / idempotent
}

// Backward upper bound: caller(100) -> mid(unprofiled) -> leaf(3). Forward
// alone would heat mid to 100, but mid calls the cold profiled leaf on its
// entry path, so the sound backward cap (count(invoke) <= call_count(leaf))
// limits mid to 3.
TEST_F(SyntheticBlockCountsTest, InterMethodBackwardUpperBoundCapsForward) {
  auto* leaf = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* mid = make_caller(leaf); // unprofiled; invokes leaf on entry path
  auto* caller = make_caller(mid); // invokes mid on entry path
  auto profiles = profiles_of({{caller, 100.0}, {leaf, 3.0}});
  auto scope = scope_of({caller, mid, leaf});

  run_inter(scope, profiles);

  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()),
                  100.0f); // pinned
  EXPECT_FLOAT_EQ(fv(leaf->get_code()->cfg().entry_block()), 3.0f); // pinned
  // mid: forward=100, capped by leaf's total to 3.
  EXPECT_FLOAT_EQ(fv(mid->get_code()->cfg().entry_block()), 3.0f);
}

// Same shape, but leaf's call_count is +inf. A non-finite count is not a usable
// backward bound: it must be skipped (not divided into hi_scale), so mid keeps
// its forward value 100 rather than being capped or poisoned with NaN/inf. The
// EXPECT_FLOAT_EQ(100) also fails if any NaN/inf leaked into mid's blocks.
TEST_F(SyntheticBlockCountsTest, InterMethodNonFiniteCalleeBoundIgnored) {
  auto* leaf = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* mid = make_caller(leaf); // unprofiled; invokes leaf on entry path
  auto* caller = make_caller(mid); // invokes mid on entry path
  auto profiles = profiles_of(
      {{caller, 100.0}, {leaf, std::numeric_limits<double>::infinity()}});
  auto scope = scope_of({caller, mid, leaf});

  run_inter(scope, profiles);

  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()),
                  100.0f); // pinned
  // Non-finite leaf bound ignored: mid stays at its forward value, uncapped.
  EXPECT_FLOAT_EQ(fv(mid->get_code()->cfg().entry_block()), 100.0f);
}

// Same shape, but leaf's call_count is NEGATIVE -- invalid profile data. It
// must not pin leaf to a negative scale and must not be used as a backward
// bound (a negative bound would drag mid's scale below 0). Both are rejected:
// leaf is forward-filled (finite, positive) and mid keeps its uncapped forward
// value.
TEST_F(SyntheticBlockCountsTest, InterMethodNegativeCalleeCountIgnored) {
  auto* leaf = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* mid = make_caller(leaf); // unprofiled; invokes leaf on entry path
  auto* caller = make_caller(mid); // invokes mid on entry path
  auto profiles = profiles_of({{caller, 100.0}, {leaf, -5.0}}); // invalid
  auto scope = scope_of({caller, mid, leaf});

  run_inter(scope, profiles);

  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()),
                  100.0f); // pinned
  // Negative leaf neither pins a negative scale nor caps mid: mid stays 100 and
  // leaf is forward-filled to a finite, positive value.
  EXPECT_FLOAT_EQ(fv(mid->get_code()->cfg().entry_block()), 100.0f);
  EXPECT_GT(fv(leaf->get_code()->cfg().entry_block()), 0.0f);
}

// Annotation-only contract: inter-method mode must not mutate the CFG. An
// unreachable block (injected via create_block, since the assembler prunes dead
// code at build) is left in place -- no dead-block removal, matching the
// per-method path and keeping the pass byte-identical on bytecode.
TEST_F(SyntheticBlockCountsTest, InterMethodDoesNotRemoveUnreachableBlocks) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  m->rstate.set_root();
  m->get_code()->cfg().create_block(); // orphan: no predecessors -> unreachable
  auto profiles = profile_with(m, 100.0);
  auto scope = scope_of({m});
  const size_t before = m->get_code()->cfg().blocks().size();
  ASSERT_GE(before, 2u); // entry + the injected unreachable block
  run_inter(scope, profiles);
  EXPECT_EQ(m->get_code()->cfg().blocks().size(), before); // nothing removed
}

// no_optimizations methods must never have their bytecode changed. Inter-method
// mode only annotates SourceBlocks, so the CFG of such a method (even with an
// unreachable block) is left intact.
TEST_F(SyntheticBlockCountsTest, InterMethodLeavesNoOptimizationsCfgUnchanged) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  m->rstate.set_root();
  m->rstate.set_no_optimizations();
  m->get_code()->cfg().create_block(); // orphan unreachable block
  auto profiles = profile_with(m, 100.0);
  auto scope = scope_of({m});
  const size_t before = m->get_code()->cfg().blocks().size();
  ASSERT_GE(before, 2u);
  run_inter(scope, profiles);
  EXPECT_EQ(m->get_code()->cfg().blocks().size(), before);
}

// True-virtual split: a profiled caller invoke-virtuals a base method with two
// overriders. The 3 resolved targets (base + 2 overriders) each receive an
// even 1/3 share of the caller's count.
TEST_F(SyntheticBlockCountsTest, InterMethodTrueVirtualUniformSplit) {
  std::string body = R"((
      (.src_block "LX;.g:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))";
  auto* base = make_vmethod("LVBase;", type::java_lang_Object(), body);
  auto* s1 = make_vmethod("LVSub1;", base->get_class(), body);
  auto* s2 = make_vmethod("LVSub2;", base->get_class(), body);
  auto* caller = make(R"((
      (.src_block "LC;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (invoke-virtual (v0) "LVBase;.g:()V")
      (return-void)
    ))");
  caller->rstate.set_root();
  auto profiles = profile_with(caller, 90.0);
  auto scope = scope_of({caller, base, s1, s2});

  run_inter(scope, profiles);

  // 3 targets, uniform split: 90 / 3 = 30 each.
  EXPECT_FLOAT_EQ(fv(base->get_code()->cfg().entry_block()), 30.0f);
  EXPECT_FLOAT_EQ(fv(s1->get_code()->cfg().entry_block()), 30.0f);
  EXPECT_FLOAT_EQ(fv(s2->get_code()->cfg().entry_block()), 30.0f);
}

// Recursion / SCC: a self-recursive unprofiled method called by a hot profiled
// caller stays BOUNDED (no divergence), converges to the geometric fixpoint,
// and is deterministic. Its self-invoke sits on a taken-half branch
// (k_self=0.5), so scale = 100 + 0.5*scale => 200.
TEST_F(SyntheticBlockCountsTest, InterMethodRecursionBoundedAndConverges) {
  auto* rec = make_named("LREC;", R"((
      (.src_block "LREC;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :done)
      (.src_block "LREC;.f:()V" 1 (1.0 1.0))
      (invoke-static () "LREC;.f:()V")
      (:done)
      (.src_block "LREC;.f:()V" 2 (1.0 1.0))
      (return-void)
    ))");
  auto* caller = make_caller(rec);
  auto profiles = profile_with(caller, 100.0);
  auto scope = scope_of({caller, rec});

  run_inter(scope, profiles);
  auto& rc = rec->get_code()->cfg();
  const float e = fv(rc.entry_block());
  EXPECT_GT(e, 100.0f); // amplified by the recursion beyond a single call
  // Bounded by the data-derived widening ceiling
  // factor(default 10.0) * max(usable call_count)(100) = 1000; the p_back=0.5
  // fixpoint (200) is well under it, so the ceiling does not bite here.
  EXPECT_LT(e, 1000.0f);
  EXPECT_NEAR(e, 200.0f, 1.0f); // geometric fixpoint 100/(1-0.5)

  auto v1 = all_vals(rc);
  run_inter(scope, profiles);
  EXPECT_EQ(v1, all_vals(rec->get_code()->cfg())); // deterministic / idempotent
}

TEST_F(SyntheticBlockCountsTest,
       InterMethodRecursionHighBackEdgePinnedAtCeiling) {
  // A recursion whose back-edge probability is ~0.99 (the `:done` exit is
  // nearly uncovered via appear100=0.01), so the geometric fixpoint
  // 100/(1-0.99) would diverge toward ~10000. The data-derived widening ceiling
  // factor(default 10.0) * max(usable call_count)(100) = 1000 pins it instead,
  // and the method is flagged as having hit the ceiling.
  auto* rec = make_named("LREC;", R"((
      (.src_block "LREC;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :done)
      (.src_block "LREC;.f:()V" 1 (1.0 1.0))
      (invoke-static () "LREC;.f:()V")
      (:done)
      (.src_block "LREC;.f:()V" 2 (1.0 0.01))
      (return-void)
    ))");
  auto* caller = make_caller(rec);
  auto profiles = profile_with(caller, 100.0);
  auto scope = scope_of({caller, rec});

  auto st = run_inter(scope, profiles);
  auto& rc = rec->get_code()->cfg();
  const float e = fv(rc.entry_block());
  // Pinned at the data-derived ceiling, not the divergent geometric fixpoint.
  EXPECT_NEAR(e, 1000.0f, 5.0f);
  EXPECT_GE(st.pairs_hit_ceiling, 1u);
}

// A deep call chain: BFS order is non-recursive (no stack overflow on depth),
// and callers-before-callees propagates the whole chain in a single sweep.
TEST_F(SyntheticBlockCountsTest, InterMethodDeepChainOneSweepNoCrash) {
  const size_t N = 2000;
  std::vector<DexMethod*> chain(N);
  for (size_t j = N; j-- > 0;) {
    std::string cls = "LCHAIN" + std::to_string(j) + ";";
    std::string body;
    if (j + 1 < N) {
      body = "(\n  (.src_block \"" + cls + ".f:()V\" 0 (1.0 1.0))\n" +
             "  (invoke-static () \"LCHAIN" + std::to_string(j + 1) +
             ";.f:()V\")\n  (return-void)\n)";
    } else {
      body = "(\n  (.src_block \"" + cls +
             ".f:()V\" 0 (1.0 1.0))\n  (const v0 0)\n  (return-void)\n)";
    }
    chain[j] = make_named(cls, body);
  }
  chain[0]->rstate.set_root();
  auto profiles = profile_with(chain[0], 100.0);
  run_inter(scope_of(chain), profiles);
  EXPECT_FLOAT_EQ(fv(chain[0]->get_code()->cfg().entry_block()), 100.0f);
  EXPECT_FLOAT_EQ(fv(chain[N - 1]->get_code()->cfg().entry_block()), 100.0f);
}

// The result is independent of the order methods are handed to the pass (the
// solve sorts internally and walks the call graph from its entry).
TEST_F(SyntheticBlockCountsTest, InterMethodDeterministicAcrossInputOrder) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profile_with(caller, 77.0);

  run_inter(scope_of({caller, callee}), profiles);
  auto v1 = all_vals(callee->get_code()->cfg());
  run_inter(scope_of({callee, caller}), profiles); // reversed input order
  auto v2 = all_vals(callee->get_code()->cfg());
  EXPECT_EQ(v1, v2);
}

// A non-finite call_count is not a usable anchor: the per-method
// solve leaves the block vals untouched (no NaN written) and reports it as a
// no-profile fallback, exactly like an absent row.
TEST_F(SyntheticBlockCountsTest, NonFiniteCallCountSkipsSlotPerMethod) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto profiles = profile_with(m, std::numeric_limits<double>::quiet_NaN());
  auto before = all_vals(m->get_code()->cfg());
  auto res = run(m, profiles);
  auto after = all_vals(m->get_code()->cfg());
  EXPECT_EQ(before, after); // untouched -- no NaN written
  EXPECT_EQ(res.solve_fallback_no_profile, 1u);
  EXPECT_EQ(res.methods_with_usable_profile, 0u);
}

// Same for the inter-method pin loop: a "profiled" method whose
// call_count is non-finite is NOT pinned, so it seeds no NaN/inf into the flow
// and nothing is forward-filled.
TEST_F(SyntheticBlockCountsTest, NonFiniteCallCountNotPinnedInterMethod) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles =
      profile_with(caller, std::numeric_limits<double>::quiet_NaN());
  auto scope = scope_of({caller, callee});

  auto st = run_inter(scope, profiles);

  EXPECT_EQ(st.pairs_pinned, 0u); // non-finite count is not a valid pin
  // Both stay boolean 1.0 -- no NaN/inf written anywhere.
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1.0f);
  EXPECT_FLOAT_EQ(fv(callee->get_code()->cfg().entry_block()), 1.0f);
}

// The clamp caps an over-cap covered block at the callee's
// profiled call_count, on BOTH producer outputs. caller(100) invokes a cold
// callee(5) on its entry block, so the solve makes entry=100 but the sound cap
// is 5.
TEST_F(SyntheticBlockCountsTest, ClampCapsOverCapBlockBothProducers) {
  const std::string leaf = R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))";
  // Per-method producer.
  auto* calleeA = make(leaf);
  auto* callerA = make_caller(calleeA);
  auto profilesA = profiles_of({{callerA, 100.0}, {calleeA, 5.0}});
  auto scopeA = scope_of({callerA, calleeA});
  run(callerA, profilesA);
  EXPECT_FLOAT_EQ(fv(callerA->get_code()->cfg().entry_block()), 100.0f);
  auto stA = clamp(scopeA, profilesA);
  EXPECT_FLOAT_EQ(fv(callerA->get_code()->cfg().entry_block()), 5.0f);
  EXPECT_GE(stA.clamp_vals_lowered, 1u);

  // Inter-method producer.
  auto* calleeB = make(leaf);
  auto* callerB = make_caller(calleeB);
  auto profilesB = profiles_of({{callerB, 100.0}, {calleeB, 5.0}});
  auto scopeB = scope_of({callerB, calleeB});
  run_inter(scopeB, profilesB);
  EXPECT_FLOAT_EQ(fv(callerB->get_code()->cfg().entry_block()), 100.0f);
  clamp(scopeB, profilesB);
  EXPECT_FLOAT_EQ(fv(callerB->get_code()->cfg().entry_block()), 5.0f);
}

// A method the producer SKIPS (no usable profile anchor) keeps its boolean
// vals: the callsite clamp must not lower them even though the method calls a
// profiled low-count callee. (Regression: the clamp used to cap covered blocks
// of producer-untouched methods, corrupting their boolean coverage.)
TEST_F(SyntheticBlockCountsTest, ClampSkipsProducerUntouchedMethod) {
  const std::string leaf = R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))";
  auto* callee = make(leaf);
  auto* caller = make_caller(callee);
  // Only the callee is profiled, with a low count that would cap the caller's
  // block well below its boolean 1.0. The caller has NO profile, so the
  // per-method producer skips it and leaves its boolean vals untouched.
  auto profiles = profiles_of({{callee, 0.3}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // producer skips the caller (no usable anchor)
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1.0f);
  const auto st = clamp(scope, profiles);
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1.0f);
  EXPECT_EQ(st.clamp_vals_lowered, 0u);
}

// Repeated-callee soundness: only GUARANTEED calls raise the
// multiplicity. The first invoke of M can itself throw, so the second invoke is
// not reached on every entry -> it is throw-gated and the cap stays
// floor(call_count(M)/1) = 100, NOT floor(100/2) = 50 (which would be an
// unsound lower bound: if the first call always threw, the block would still
// run 1000x).
TEST_F(SyntheticBlockCountsTest,
       ClampRepeatedThrowingCalleeCountsGuaranteedOnce) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(callee) + "\")\n  (invoke-static () \"" + show(callee) +
      "\")\n  (return-void)\n)";
  auto* caller = make(body);
  caller->rstate.set_root();
  auto profiles = profiles_of({{caller, 1000.0}, {callee, 100.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // entry = 1000
  auto st = clamp(scope, profiles);
  // Guaranteed multiplicity is 1 (second invoke throw-gated), cap = 100.
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 100.0f);
  EXPECT_GE(st.clamp_throw_gated_invokes, 1u);
}

// Soundness: a may-throw instruction (here a div, which may
// raise ArithmeticException) BEFORE the sole invoke means the callee is not
// reached on every entry -- so its call_count is not a sound cap. The invoke is
// throw-gated and the over-cap block is left untouched (fail-open), rather than
// clamped down to an unsound bound.
TEST_F(SyntheticBlockCountsTest, ClampThrowGatesCalleeAfterMayThrow) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v1 10)\n  (const v2 2)\n  (div-int v1 v2)\n"
      "  (move-result-pseudo v0)\n"
      "  (invoke-static () \"" +
      show(callee) + "\")\n  (return-void)\n)";
  auto* caller = make(body);
  caller->rstate.set_root();
  auto profiles = profiles_of({{caller, 1000.0}, {callee, 100.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // entry = 1000
  auto st = clamp(scope, profiles);
  // Callee throw-gated: no sound cap, entry stays at its synthesized 1000.
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1000.0f);
  EXPECT_GE(st.clamp_throw_gated_invokes, 1u);
}

// cap==0 on a covered block resolves to epsilon (support pin
// wins over the zero cap) -- it is lowered but never to 0.
TEST_F(SyntheticBlockCountsTest, ClampZeroCapFloorsToEpsilon) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 0.0}}); // callee cold
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // entry = 100
  clamp(scope, profiles);
  const float e = fv(caller->get_code()->cfg().entry_block());
  EXPECT_GT(e, 0.0f); // support pin: covered block stays > 0
  EXPECT_LT(e, 1.0f); // but it WAS lowered from 100 toward the zero cap
}

// A negative callee call_count is invalid profile data, not a
// usable cap: it is ignored (fail-open), leaving the covered block untouched,
// rather than lowering it to epsilon from a negative bound. (Contrast
// ClampZeroCapFloorsToEpsilon, where a valid 0 count DOES lower to epsilon.)
TEST_F(SyntheticBlockCountsTest, ClampNegativeCalleeCountIgnored) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profiles_of({{caller, 100.0}, {callee, -5.0}}); // invalid
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // entry = 100
  clamp(scope, profiles);
  // Negative callee bound ignored: entry keeps its synthesized 100, not
  // epsilon.
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 100.0f);
}

// Deterministic + idempotent: a second clamp changes nothing.
TEST_F(SyntheticBlockCountsTest, ClampDeterministicIdempotent) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 5.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles);
  clamp(scope, profiles);
  auto v1 = all_vals(caller->get_code()->cfg());
  clamp(scope, profiles);
  auto v2 = all_vals(caller->get_code()->cfg());
  EXPECT_EQ(v1, v2);
}

// The real win: flow freed from an over-capped arm is
// rerouted to a hot sibling with headroom, not truncated. A->{B(cap 10),
// C(uncapped)}: baseline splits 50/50; re-flow leaves B=10 and moves the freed
// 40 to C=90.
TEST_F(SyntheticBlockCountsTest, ReflowReroutesFreedFlowToHotSibling) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (if-eqz v0 :cee)\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(callee) +
      "\")\n" // B: capped by callee(10)
      "  (goto :end)\n"
      "  (:cee)\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (const v1 1)\n" // C: uncapped
      "  (goto :end)\n"
      "  (:end)\n"
      "  (.src_block \"LC;.f:()V\" 3 (1.0 1.0))\n"
      "  (return-void)\n)";
  auto* caller = make(body);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 10.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // baseline: entry=100, arms=50/50
  auto st = reflow(scope, profiles);
  auto& cfg = caller->get_code()->cfg();
  EXPECT_FLOAT_EQ(fv(cfg.entry_block()), 100.0f); // entry pinned
  auto arms = arm_vals(cfg.entry_block());
  ASSERT_EQ(arms.size(), 2u);
  std::sort(arms.begin(), arms.end());
  EXPECT_FLOAT_EQ(arms[0], 10.0f); // capped arm
  EXPECT_FLOAT_EQ(arms[1], 90.0f); // sibling absorbed the freed 40
  EXPECT_GE(st.reflow_blocks_capped, 1u);
}

// Re-flow must conserve flow when TWO finite-headroom siblings can jointly
// absorb a capped block's freed flow -- it must not spill what fits. Regression
// for the non-proportional pass-2 allocation that multiplied the *shrinking*
// leftover by the *original* rem_finite proportion, under-allocating the second
// sibling and falsely spilling. The absorbers are FINITE (not the +inf fast
// path the other reflow test exercises).
TEST_F(SyntheticBlockCountsTest, ReflowSpillsNothingWhenTwoFiniteSiblingsFit) {
  auto* cold = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* absA = make(R"((
      (.src_block "LY;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* absB = make(R"((
      (.src_block "LZ;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  // entry switches 3 ways: default arm calls cold (capped low, frees flow); the
  // two case arms call absA/absB (finite caps with ample joint headroom).
  const std::string body =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (switch v0 (:a :b))\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(cold) +
      "\")\n" // default arm: capped by cold(6)
      "  (goto :end)\n"
      "  (:a)\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(absA) +
      "\")\n" // capped by absA(60)
      "  (goto :end)\n"
      "  (:b)\n"
      "  (.src_block \"LC;.f:()V\" 3 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(absB) +
      "\")\n" // capped by absB(60)
      "  (goto :end)\n"
      "  (:end)\n"
      "  (.src_block \"LC;.f:()V\" 4 (1.0 1.0))\n"
      "  (return-void)\n)";
  auto* caller = make(body);
  auto profiles =
      profiles_of({{caller, 90.0}, {cold, 6.0}, {absA, 60.0}, {absB, 60.0}});
  auto scope = scope_of({caller, cold, absA, absB});
  run(caller, profiles); // baseline: entry=90, three arms ~30 each
  auto st = reflow(scope, profiles);
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 90.0f); // pinned
  // Default arm capped at 6 frees ~24; absA+absB headroom (~60) easily fits it,
  // so nothing spills. The bug would falsely spill part of the freed flow.
  EXPECT_FLOAT_EQ(st.reflow_sink_spill_total, 0.0);
}

// A per-block execution cap inside a loop must be converted to SCC-ENTRY-flow
// units before it bounds the SCC. A self-loop head runs ~2x per entry (p_back
// 0.5 -> amplified to 200 at anchor 100) and calls a callee capped at 50. The
// cap is on head EXECUTIONS, so the loop's entry cap is 50 * 100/200 = 25
// entries, which keeps head at 50. The buggy raw-cap code compared the block
// cap (50) directly against entry flow (100), leaving head at 100 and
// under-enforcing the callee.
TEST_F(SyntheticBlockCountsTest, ReflowConvertsLoopBlockCapToEntryUnits) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (:head)\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(callee) +
      "\")\n" // head: runs ~2x/entry, capped by callee(50)
      "  (if-eqz v0 :head)\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (return-void)\n)";
  auto* caller = make(body);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 50.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles);
  auto& cfg = caller->get_code()->cfg();
  auto* head = single_succ(cfg.entry_block());
  ASSERT_NE(head, nullptr);
  EXPECT_FLOAT_EQ(fv(head), 200.0f); // baseline geometric amplification
  reflow(scope, profiles);
  EXPECT_FLOAT_EQ(fv(cfg.entry_block()), 100.0f); // entry pinned
  EXPECT_FLOAT_EQ(fv(head),
                  50.0f); // converted cap; raw-cap bug leaves it at 100
}

// Entry is PINNED: even when the entry block invokes a cold
// callee, re-flow does NOT truncate the entry below the method's anchor (that
// is the clamp's job, which runs after). A(1000) invoking callee(100) -> A
// stays 1000 after re-flow.
TEST_F(SyntheticBlockCountsTest, ReflowPinsEntryNoDeAnchor) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profiles_of({{caller, 1000.0}, {callee, 100.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles);
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1000.0f);
  reflow(scope, profiles);
  // Re-flow must not de-anchor the entry (Fix #12); the clamp would cap it.
  EXPECT_FLOAT_EQ(fv(caller->get_code()->cfg().entry_block()), 1000.0f);
}

// Regression guard for loop-latch zeroing (Fix #10): a loop
// header with no binding cap keeps its geometric-amplified count after re-flow,
// it is NOT floored to epsilon.
TEST_F(SyntheticBlockCountsTest, ReflowDoesNotZeroLoopLatch) {
  auto* m = make(R"((
      (.src_block "LD;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (:head)
      (.src_block "LD;.f:()V" 1 (1.0 1.0))
      (if-eqz v0 :head)
      (.src_block "LD;.f:()V" 2 (1.0 1.0))
      (return-void)
    ))");
  auto profiles = profile_with(m, 100.0);
  run(m, profiles); // head amplified to 200 (p_back 0.5 -> x2)
  auto scope = scope_of({m});
  auto& cfg = m->get_code()->cfg();
  auto* head = single_succ(cfg.entry_block());
  ASSERT_NE(head, nullptr);
  EXPECT_FLOAT_EQ(fv(head), 200.0f);
  reflow(scope, profiles);
  EXPECT_FLOAT_EQ(fv(cfg.entry_block()), 100.0f); // entry pinned
  EXPECT_FLOAT_EQ(fv(head), 200.0f); // header NOT zeroed (no cap binds)
}

// Capped diamond: the profile is internally contradictory
// (everything funnels through a capped merge, but the entry is pinned). Re-flow
// routes what it can and honestly spills the rest (metered); the merge ends at
// or below its cap.
TEST_F(SyntheticBlockCountsTest, ReflowCappedDiamondSpills) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (if-eqz v0 :right)\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (const v1 1)\n"
      "  (goto :merge)\n"
      "  (:right)\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (const v1 2)\n"
      "  (goto :merge)\n"
      "  (:merge)\n"
      "  (.src_block \"LC;.f:()V\" 3 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(callee) +
      "\")\n" // merge D: capped by callee(10)
      "  (return-void)\n)";
  auto* caller = make(body);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 10.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles); // baseline: entry=100, arms=50/50, merge=100
  auto st = reflow(scope, profiles);
  auto& cfg = caller->get_code()->cfg();
  EXPECT_FLOAT_EQ(fv(cfg.entry_block()), 100.0f); // entry pinned
  auto arms = arm_vals(cfg.entry_block());
  ASSERT_EQ(arms.size(), 2u);
  auto* merge = single_succ(cfg.entry_block()->succs().front()->target());
  ASSERT_NE(merge, nullptr);
  EXPECT_LE(fv(merge), 10.0f + 1e-3f); // funnel bound honored
  EXPECT_GT(st.reflow_sink_spill_total, 0.0); // contradiction spilled, metered
  EXPECT_EQ(st.reflow_methods_spilled, 1u);
}

// Deterministic + idempotent: a second re-flow (entry pinned,
// recomputed from the same anchor) produces identical values.
TEST_F(SyntheticBlockCountsTest, ReflowDeterministicIdempotent) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  auto* caller = make_caller(callee);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 5.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles);
  reflow(scope, profiles);
  auto v1 = all_vals(caller->get_code()->cfg());
  reflow(scope, profiles);
  auto v2 = all_vals(caller->get_code()->cfg());
  EXPECT_EQ(v1, v2);
}

// Escape via THROW (not just return): a covered block whose terminator is an
// explicit `throw` caught by a handler has the handler as an inter-SCC
// successor
// -- so it is NOT a condensation sink, and only `block_exits`' throw case makes
// it escape. The handler here is capped (call_count 5); without throw-as-escape
// the throwing block would be wrongly bounded by that cap instead of shedding
// its flow out the throw. It should keep ~its entry flow.
TEST_F(SyntheticBlockCountsTest, ReflowThrowIsAnEscape) {
  auto* callee = make(R"((
      (.src_block "LX;.f:()V" 0 (1.0 1.0))
      (const v0 0)
      (return-void)
    ))");
  const std::string body =
      "(\n"
      "  (.src_block \"LC;.f:()V\" 0 (1.0 1.0))\n"
      "  (const v0 0)\n"
      "  (.try_start t)\n"
      "  (.src_block \"LC;.f:()V\" 1 (1.0 1.0))\n"
      "  (throw v0)\n"
      "  (.try_end t)\n"
      "  (.catch (t))\n"
      "  (.src_block \"LC;.f:()V\" 2 (1.0 1.0))\n"
      "  (invoke-static () \"" +
      show(callee) +
      "\")\n"
      "  (return-void)\n)";
  auto* caller = make(body);
  auto profiles = profiles_of({{caller, 100.0}, {callee, 5.0}});
  auto scope = scope_of({caller, callee});
  run(caller, profiles);
  reflow(scope, profiles);
  auto& cfg = caller->get_code()->cfg();
  EXPECT_FLOAT_EQ(fv(cfg.entry_block()), 100.0f); // entry pinned
  cfg::Block* thrower = nullptr;
  for (auto* b : cfg.blocks()) {
    auto it = b->get_last_insn();
    if (it != b->end() && it->insn->opcode() == OPCODE_THROW) {
      thrower = b;
    }
  }
  ASSERT_NE(thrower, nullptr);
  // Escapes via the throw -> keeps its flow, not clamped to the handler cap
  // (5).
  EXPECT_GT(fv(thrower), 5.0f);
}
