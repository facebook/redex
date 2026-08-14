/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SyntheticBlockCounts.h"

#include <atomic>
#include <map>
#include <string>

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

  // Fixture is the friend; TEST_F bodies (a derived class) are not, so private
  // members are reached through helpers like this.
  std::vector<int64_t> missing_hit(
      const Scope& scope,
      const method_profiles::MethodProfiles& profiles,
      const std::vector<std::string>& inv_slot = {"Fake"}) {
    return m_pass.count_missing_hit_methods(scope, profiles, inv_slot);
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
