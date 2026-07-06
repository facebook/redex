/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IROpcode.h"
#include "InitClassesWithSideEffects.h"
#include "MethodUtil.h"
#include "Purity.h"
#include "RedexTest.h"
#include "StringSwitchFinder.h"
#include "StringSwitchTestUtil.h"
#include "StringSwitchTransform.h"
#include "StringTreeMapTransform.h"
#include "TypeUtil.h"

namespace {

namespace cp = constant_propagation;

// A test double: reports a fixed score and counts apply() calls. apply() is a
// no-op on the CFG (so the recovered switch survives) -- the driver's budget
// cap must still guarantee it is applied at most once per recovered switch.
class MockTransform : public StringSwitchTransform {
 public:
  MockTransform(std::string name,
                std::optional<TransformScore> score,
                int* applied_counter)
      : m_name(std::move(name)),
        m_score(score),
        m_applied_counter(applied_counter) {}

  std::string_view name() const override { return m_name; }

  std::optional<TransformScore> evaluate(
      const StringSwitchCandidate& /*candidate*/) const override {
    return m_score;
  }

  void apply(const StringSwitchCandidate& /*candidate*/) const override {
    ++*m_applied_counter;
  }

 private:
  std::string m_name;
  std::optional<TransformScore> m_score;
  int* m_applied_counter;
};

// Runs the driver on `cfg`, supplying the (empty-scope) init-class info and
// side-effect-free method set that the pass builds once in production.
void run_driver(
    cfg::ControlFlowGraph& cfg,
    const std::vector<std::unique_ptr<StringSwitchTransform>>& transforms,
    DriverStats* stats) {
  init_classes::InitClassesWithSideEffects init_classes(
      /*scope=*/{}, /*create_init_class_insns=*/false);
  run_string_switch_transforms(/*method=*/nullptr, cfg, transforms,
                               get_pure_methods(), init_classes, stats);
}

// A minimal recoverable Form B (equals-chain) string switch.
std::unique_ptr<IRCode> two_case_switch() {
  return assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-static (v1) "Lfoo;.before:(Ljava/lang/Object;)I")
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (invoke-static (v1) "Lfoo;.after:(Ljava/lang/Object;)I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :first)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:second)
      (const-string "RES_second")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )");
}

// As above but with a non-ASCII case label, which the StringTreeMap encoder
// cannot represent.
std::unique_ptr<IRCode> non_ascii_switch() {
  return assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "café")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :first)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:second)
      (const-string "RES_second")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )");
}

// A canonical two-stage HASH_SWITCH over {"abc","xyz"} with real java
// hashCodes, plus unrelated invokes before and after the hashCode() to verify
// they survive the rewrite.
std::unique_ptr<IRCode> hash_switch_two_cases() {
  return assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-static (v3) "Lfoo;.before:(Ljava/lang/Object;)I")
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (invoke-static (v3) "Lfoo;.after:(Ljava/lang/Object;)I")
      (const v1 -1)
      (switch v0 (:habc :hxyz))

      (:ord)
      (switch v1 (:body0 :body1))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:habc 96354)
      (const-string "abc")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
      (goto :ord)

      (:hxyz 119193)
      (const-string "xyz")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set1)
      (goto :ord)
      (:set1)
      (const v1 1)
      (goto :ord)

      (:body0 0)
      (const-string "RES_abc")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:body1 1)
      (const-string "RES_xyz")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
}

// hash_switch_two_cases() wrapped in a try/catch with a single Exception
// handler, to exercise throw-edge reattachment after the rewrite.
std::unique_ptr<IRCode> hash_switch_two_cases_in_try() {
  return assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (.try_start tc)
      (invoke-static (v3) "Lfoo;.before:(Ljava/lang/Object;)I")
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (invoke-static (v3) "Lfoo;.after:(Ljava/lang/Object;)I")
      (const v1 -1)
      (switch v0 (:habc :hxyz))

      (:ord)
      (switch v1 (:body0 :body1))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:habc 96354)
      (const-string "abc")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
      (goto :ord)

      (:hxyz 119193)
      (const-string "xyz")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set1)
      (goto :ord)
      (:set1)
      (const v1 1)
      (goto :ord)

      (:body0 0)
      (const-string "RES_abc")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:body1 1)
      (const-string "RES_xyz")
      (move-result-pseudo-object v4)
      (return-object v4)
      (.try_end tc)

      (.catch (tc) "Ljava/lang/Exception;")
      (move-exception v5)
      (const-string "RES_caught")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
}

// The unrelated invokes the generators place around the dispatch, plus the
// configured StringTreeSet-style lookup searchMap(name, data, notFound) -> i.
constexpr const char* kBeforeMethod = "Lfoo;.before:(Ljava/lang/Object;)I";
constexpr const char* kAfterMethod = "Lfoo;.after:(Ljava/lang/Object;)I";
constexpr const char* kLookupMethod =
    "Lcom/foo/StringTreeSet;.searchMap:(Ljava/lang/String;Ljava/lang/"
    "String;I)I";

DexMethodRef* make_lookup_method() {
  return DexMethod::make_method(kLookupMethod);
}

// LocalDce only removes an invoke whose method resolves; java.lang.String is
// not loaded in unit tests, so give it concrete hashCode/equals defs to resolve
// to.
void ensure_string_methods_resolvable() {
  auto* string_type = type::java_lang_String();
  if (type_class(string_type) == nullptr) {
    ClassCreator creator(string_type);
    creator.set_super(type::java_lang_Object());
    creator.create();
  }
  auto* cls = type_class(string_type);
  auto define = [&](DexMethodRef* ref) {
    if (ref->is_def()) {
      return;
    }
    cls->add_method(ref->make_concrete(ACC_PUBLIC, /*is_virtual=*/true));
  };
  define(method::java_lang_String_hashCode());
  define(method::java_lang_String_equals());
}

// The first const-string literal in `block`, or "" if none.
std::string first_string_literal(cfg::Block* block) {
  for (auto& mie : InstructionIterable(block)) {
    if (mie.insn->opcode() == OPCODE_CONST_STRING) {
      return std::string(mie.insn->get_string()->str());
    }
  }
  return "";
}

// The destination strings reached from a switch block: each branch target's and
// the goto target's first const-string literal.
std::set<std::string> switch_destinations(cfg::Block* switch_block) {
  std::set<std::string> dests;
  for (auto* e : switch_block->succs()) {
    if (e->type() == cfg::EDGE_BRANCH || e->type() == cfg::EDGE_GOTO) {
      dests.insert(first_string_literal(e->target()));
    }
  }
  return dests;
}

std::shared_ptr<cp::intraprocedural::FixpointIterator> make_fixpoint(
    cfg::ControlFlowGraph& cfg) {
  auto fixpoint = std::make_shared<cp::intraprocedural::FixpointIterator>(
      cfg, StringSwitchFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());
  return fixpoint;
}

// Counts invoke-static instructions whose target is the method `signature`.
size_t count_invoke_static_to(cfg::ControlFlowGraph& cfg,
                              const std::string& signature) {
  auto* ref = DexMethod::get_method(signature);
  size_t n = 0;
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC &&
        mie.insn->get_method() == ref) {
      n++;
    }
  }
  return n;
}

} // namespace

struct StringSwitchTransformTest : public RedexTest {
  StringSwitchTransformTest() { ensure_string_methods_resolvable(); }

  // Recovers `code`'s single string switch and applies the StringTreeMap
  // transform to it via the driver, asserting it applied exactly once.
  void apply_tree_map(IRCode* code) {
    std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
    transforms.push_back(std::make_unique<StringTreeMapTransform>(
        make_lookup_method(), /*min_cases=*/2, /*max_payload_size=*/8000));
    DriverStats stats;
    run_driver(code->cfg(), transforms, &stats);
    EXPECT_EQ(stats.applied["string_tree_map"], 1u);
  }
};

TEST_F(StringSwitchTransformTest, is_better_prefers_performance_tier) {
  TransformScore size_big{TransformTier::SIZE, 1000};
  TransformScore perf_small{TransformTier::PERFORMANCE, 1};
  // PERFORMANCE beats SIZE regardless of magnitude.
  EXPECT_TRUE(is_better(perf_small, size_big));
  EXPECT_FALSE(is_better(size_big, perf_small));
}

TEST_F(StringSwitchTransformTest, is_better_breaks_ties_by_magnitude) {
  TransformScore hi{TransformTier::SIZE, 10};
  TransformScore lo{TransformTier::SIZE, 5};
  EXPECT_TRUE(is_better(hi, lo));
  EXPECT_FALSE(is_better(lo, hi));
  EXPECT_FALSE(is_better(hi, hi)); // strict
}

TEST_F(StringSwitchTransformTest, driver_picks_higher_tier) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied_size = 0;
  int applied_perf = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(std::make_unique<MockTransform>(
      "size", TransformScore{TransformTier::SIZE, 1000}, &applied_size));
  transforms.push_back(std::make_unique<MockTransform>(
      "perf", TransformScore{TransformTier::PERFORMANCE, 1}, &applied_perf));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied_perf, 1);
  EXPECT_EQ(applied_size, 0);
  EXPECT_EQ(stats.applied["perf"], 1u);
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_picks_higher_magnitude_within_tier) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied_hi = 0;
  int applied_lo = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(std::make_unique<MockTransform>(
      "lo", TransformScore{TransformTier::SIZE, 5}, &applied_lo));
  transforms.push_back(std::make_unique<MockTransform>(
      "hi", TransformScore{TransformTier::SIZE, 10}, &applied_hi));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied_hi, 1);
  EXPECT_EQ(applied_lo, 0);
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_skips_when_not_applicable) {
  auto code = two_case_switch();
  code->build_cfg();
  int applied = 0;
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  transforms.push_back(
      std::make_unique<MockTransform>("none", std::nullopt, &applied));

  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);

  EXPECT_EQ(applied, 0);
  EXPECT_TRUE(stats.applied.empty());
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, driver_noop_with_empty_registry) {
  auto code = two_case_switch();
  code->build_cfg();
  std::vector<std::unique_ptr<StringSwitchTransform>> transforms;
  DriverStats stats;
  run_driver(code->cfg(), transforms, &stats);
  EXPECT_TRUE(stats.applied.empty());
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, tree_map_evaluate_eligible) {
  auto code = two_case_switch();
  code->build_cfg();
  auto& cfg = code->cfg();
  StringSwitchCfgContext ctx(cfg, make_fixpoint(cfg));
  auto switches = find_string_switches(ctx);
  ASSERT_EQ(switches.size(), 1u);

  StringTreeMapTransform transform(make_lookup_method(), /*min_cases=*/2,
                                   /*max_payload_size=*/8000);
  StringSwitchCandidate candidate{nullptr, ctx, switches[0]};
  auto score = transform.evaluate(candidate);
  ASSERT_TRUE(score.has_value());
  // SIZE tier, with a no-profiling (cold) origin block.
  EXPECT_EQ(score->tier, TransformTier::SIZE);
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, tree_map_rejects_too_few_cases) {
  auto code = two_case_switch();
  code->build_cfg();
  auto& cfg = code->cfg();
  StringSwitchCfgContext ctx(cfg, make_fixpoint(cfg));
  auto switches = find_string_switches(ctx);
  ASSERT_EQ(switches.size(), 1u);

  // 3 cases (incl. default) < min_cases of 5.
  StringTreeMapTransform transform(make_lookup_method(), /*min_cases=*/5, 8000);
  StringSwitchCandidate candidate{nullptr, ctx, switches[0]};
  EXPECT_FALSE(transform.evaluate(candidate).has_value());
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, tree_map_rejects_non_ascii_keys) {
  auto code = non_ascii_switch();
  code->build_cfg();
  auto& cfg = code->cfg();
  StringSwitchCfgContext ctx(cfg, make_fixpoint(cfg));
  auto switches = find_string_switches(ctx);
  ASSERT_EQ(switches.size(), 1u);

  StringTreeMapTransform transform(make_lookup_method(), /*min_cases=*/2, 8000);
  StringSwitchCandidate candidate{nullptr, ctx, switches[0]};
  EXPECT_FALSE(transform.evaluate(candidate).has_value());
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, tree_map_rejects_oversized_payload) {
  auto code = two_case_switch();
  code->build_cfg();
  auto& cfg = code->cfg();
  StringSwitchCfgContext ctx(cfg, make_fixpoint(cfg));
  auto switches = find_string_switches(ctx);
  ASSERT_EQ(switches.size(), 1u);

  // A tiny payload cap forces the encoded trie over budget.
  StringTreeMapTransform transform(make_lookup_method(), /*min_cases=*/2,
                                   /*max_payload_size=*/1);
  StringSwitchCandidate candidate{nullptr, ctx, switches[0]};
  EXPECT_FALSE(transform.evaluate(candidate).has_value());
  code->clear_cfg();
}

// Finds the single switch block in `cfg`, asserting there is exactly one.
cfg::Block* single_switch_block(cfg::ControlFlowGraph& cfg) {
  cfg::Block* switch_block = nullptr;
  for (auto* b : cfg.blocks()) {
    auto last = b->get_last_insn();
    if (last != b->end() && last->insn->opcode() == OPCODE_SWITCH) {
      EXPECT_EQ(switch_block, nullptr) << "more than one switch block";
      switch_block = b;
    }
  }
  return switch_block;
}

// Counts the switch block's outgoing branch and goto edges.
void count_switch_edges(cfg::Block* switch_block,
                        size_t* branch_edges,
                        size_t* goto_edges) {
  *branch_edges = 0;
  *goto_edges = 0;
  for (auto* e : switch_block->succs()) {
    if (e->type() == cfg::EDGE_BRANCH) {
      (*branch_edges)++;
    } else if (e->type() == cfg::EDGE_GOTO) {
      (*goto_edges)++;
    }
  }
}

// The block containing the (single) invoke-static of `signature`, or null.
cfg::Block* block_of_invoke_static_to(cfg::ControlFlowGraph& cfg,
                                      const std::string& signature) {
  auto* ref = DexMethod::get_method(signature);
  for (auto* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      if (mie.insn->opcode() == OPCODE_INVOKE_STATIC &&
          mie.insn->get_method() == ref) {
        return b;
      }
    }
  }
  return nullptr;
}

// A block's outgoing throw edges.
std::vector<cfg::Edge*> throw_edges(cfg::Block* block) {
  std::vector<cfg::Edge*> edges;
  for (auto* e : block->succs()) {
    if (e->type() == cfg::EDGE_THROW) {
      edges.push_back(e);
    }
  }
  return edges;
}

// The opcode of a block's last instruction (or a NOP sentinel if empty).
IROpcode last_opcode(cfg::Block* block) {
  auto it = block->get_last_insn();
  return it == block->end() ? OPCODE_NOP : it->insn->opcode();
}

// The opcode of a block's first instruction (or a NOP sentinel if empty).
IROpcode first_opcode(cfg::Block* block) {
  for (auto& mie : InstructionIterable(block)) {
    return mie.insn->opcode();
  }
  return OPCODE_NOP;
}

// The single predecessor of `block` reached by a goto edge, or null.
cfg::Block* goto_predecessor(cfg::Block* block) {
  cfg::Block* pred = nullptr;
  for (auto* e : block->preds()) {
    if (e->type() == cfg::EDGE_GOTO) {
      if (pred != nullptr) {
        return nullptr; // not unique
      }
      pred = e->src();
    }
  }
  return pred;
}

// Asserts `block` carries exactly one throw edge to the Exception handler whose
// body returns "RES_caught" (catch index 0).
void expect_single_exception_edge(cfg::Block* block) {
  auto throws = throw_edges(block);
  ASSERT_EQ(throws.size(), 1u);
  EXPECT_EQ(throws[0]->throw_info()->catch_type,
            DexType::get_type("Ljava/lang/Exception;"));
  EXPECT_EQ(throws[0]->throw_info()->index, 0u);
  EXPECT_TRUE(throws[0]->target()->is_catch());
  EXPECT_EQ(first_string_literal(throws[0]->target()), "RES_caught");
}

// Asserts the canonical post-rewrite shape shared by every successful
// StringTreeMap transform: the hashCode/equals machinery is gone, the
// before/after invokes and the single lookup invoke survive, and the lone
// switch has one case per distinct destination plus a default goto reaching
// exactly `expected_dests`. Also confirms the switch is no longer recoverable.
void verify_tree_map_shape(cfg::ControlFlowGraph& cfg,
                           const std::set<std::string>& expected_dests) {
  auto counts = string_switch_test::count_string_switch_opcodes(
      cfg::InstructionIterable(cfg));
  // The hashCode/equals dispatch machinery is gone (LocalDce removed the now
  // unused pure invokes).
  EXPECT_EQ(counts.hashcode, 0u);
  EXPECT_EQ(counts.equals, 0u);
  EXPECT_EQ(counts.switches, 1u);
  // Exactly the lookup invoke plus the unrelated before/after invokes survive
  // -- the rewrite must NOT discard the `after` invoke that sits between
  // hashCode() and the branch.
  EXPECT_EQ(counts.invoke_static, 3u);
  EXPECT_EQ(count_invoke_static_to(cfg, kBeforeMethod), 1u);
  EXPECT_EQ(count_invoke_static_to(cfg, kAfterMethod), 1u);
  EXPECT_EQ(count_invoke_static_to(cfg, kLookupMethod), 1u);

  // One switch case per distinct destination, plus a goto to the default body.
  auto* switch_block = single_switch_block(cfg);
  ASSERT_NE(switch_block, nullptr);
  size_t branch_edges = 0;
  size_t goto_edges = 0;
  count_switch_edges(switch_block, &branch_edges, &goto_edges);
  EXPECT_EQ(branch_edges, 2u);
  EXPECT_EQ(goto_edges, 1u);
  EXPECT_EQ(switch_destinations(switch_block), expected_dests);

  // The recovered switch is gone, so a second pass finds nothing to do.
  StringSwitchCfgContext ctx(cfg, make_fixpoint(cfg));
  EXPECT_TRUE(find_string_switches(ctx).empty());
}

TEST_F(StringSwitchTransformTest, tree_map_apply_equals_chain_via_driver) {
  auto code = two_case_switch();
  code->build_cfg();
  apply_tree_map(code.get());
  verify_tree_map_shape(
      code->cfg(),
      std::set<std::string>{"RES_first", "RES_second", "RES_default"});
  code->clear_cfg();
}

TEST_F(StringSwitchTransformTest, tree_map_apply_hash_switch_via_driver) {
  auto code = hash_switch_two_cases();
  code->build_cfg();
  apply_tree_map(code.get());
  verify_tree_map_shape(
      code->cfg(), std::set<std::string>{"RES_abc", "RES_xyz", "RES_default"});
  code->clear_cfg();
}

// The try/catch case: each throwing instruction must end its block, so the
// rewrite spreads the lookup across three blocks
//   [...; const-string] -> [move-result-pseudo; const 0; invoke] ->
//   [move-result; switch]
// with the two throwing blocks carrying edges to the catch handler --
// exercising apply()'s throw-edge reattachment and block splitting.
TEST_F(StringSwitchTransformTest,
       tree_map_apply_hash_switch_in_try_via_driver) {
  auto code = hash_switch_two_cases_in_try();
  code->build_cfg();
  apply_tree_map(code.get());

  auto& cfg = code->cfg();
  // Same core shape as the non-try hash switch (destinations exclude the
  // handler body, which is reached via a throw edge, not a switch case).
  verify_tree_map_shape(
      cfg, std::set<std::string>{"RES_abc", "RES_xyz", "RES_default"});

  // Block 2: the lookup invoke ends its own block (a throwing instruction must
  // be last), which routes exceptions to the handler and falls through to the
  // switch block.
  auto* invoke_block = block_of_invoke_static_to(cfg, kLookupMethod);
  ASSERT_NE(invoke_block, nullptr);
  EXPECT_EQ(last_opcode(invoke_block), OPCODE_INVOKE_STATIC);
  EXPECT_EQ(first_opcode(invoke_block), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  expect_single_exception_edge(invoke_block);

  // Block 3: the (non-throwing) move-result of the lookup shares the switch
  // block, which itself carries no throw edge.
  auto* switch_block = single_switch_block(cfg);
  ASSERT_NE(switch_block, nullptr);
  EXPECT_EQ(invoke_block->goes_to(), switch_block);
  EXPECT_EQ(first_opcode(switch_block), OPCODE_MOVE_RESULT);
  EXPECT_TRUE(throw_edges(switch_block).empty());

  // Block 1: the payload const-string also ends its own block (it can throw),
  // routes to the same handler, and falls through to the invoke block.
  auto* const_string_block = goto_predecessor(invoke_block);
  ASSERT_NE(const_string_block, nullptr);
  EXPECT_EQ(last_opcode(const_string_block), OPCODE_CONST_STRING);
  expect_single_exception_edge(const_string_block);

  code->clear_cfg();
}

// A switch with a region constant (v10 = 777, set alongside the ordinal) that
// escapes into the "one" body. apply() must haul that const into the body
// before excising the region, so the body's use(v10) survives. The switch is
// transformed normally (extra_loads is no longer a rejection reason).
TEST_F(StringSwitchTransformTest, tree_map_apply_with_extra_load_via_driver) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (const v10 0)
      (switch v0 (:hone :htwo :hthree))

      (:ord)
      (switch v1 (:body0 :body1 :body2))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:hone 110182)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
      (const v10 777)
      (goto :ord)

      (:htwo 115276)
      (const-string "two")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set1)
      (goto :ord)
      (:set1)
      (const v1 1)
      (goto :ord)

      (:hthree 110339486)
      (const-string "three")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set2)
      (goto :ord)
      (:set2)
      (const v1 2)
      (goto :ord)

      (:body0 0)
      (invoke-static (v10) "Lfoo;.use:(I)V")
      (const-string "RES_one")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:body1 1)
      (const-string "RES_two")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:body2 2)
      (const-string "RES_three")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  apply_tree_map(code.get());

  auto& cfg = code->cfg();
  auto counts = string_switch_test::count_string_switch_opcodes(
      cfg::InstructionIterable(cfg));
  // The dispatch machinery is gone; the lookup feeds one switch, and the body's
  // own use(v10) survives.
  EXPECT_EQ(counts.hashcode, 0u);
  EXPECT_EQ(counts.equals, 0u);
  EXPECT_EQ(counts.switches, 1u);
  EXPECT_EQ(count_invoke_static_to(cfg, kLookupMethod), 1u);
  EXPECT_EQ(count_invoke_static_to(cfg, "Lfoo;.use:(I)V"), 1u);

  // The escaping const was hauled to the front of the body that uses it: `const
  // v10 777` immediately precedes `use(v10)`.
  auto* use_block = block_of_invoke_static_to(cfg, "Lfoo;.use:(I)V");
  ASSERT_NE(use_block, nullptr);
  auto* use_method = DexMethod::get_method("Lfoo;.use:(I)V");
  IRInstruction* prev = nullptr;
  bool found_use = false;
  for (auto& mie : InstructionIterable(use_block)) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_STATIC &&
        insn->get_method() == use_method) {
      ASSERT_NE(prev, nullptr);
      EXPECT_EQ(prev->opcode(), OPCODE_CONST);
      EXPECT_EQ(prev->dest(), 10u);
      EXPECT_EQ(prev->get_literal(), 777);
      found_use = true;
      break;
    }
    prev = insn;
  }
  EXPECT_TRUE(found_use);

  code->clear_cfg();
}
