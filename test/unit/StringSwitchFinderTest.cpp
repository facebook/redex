/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <variant>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "StringSwitchFinder.h"

namespace cp = constant_propagation;

namespace {

std::shared_ptr<cp::intraprocedural::FixpointIterator> make_fixpoint(
    cfg::ControlFlowGraph& cfg) {
  auto fp = std::make_shared<cp::intraprocedural::FixpointIterator>(
      cfg, StringSwitchFinder::Analyzer());
  fp->run(ConstantEnvironment());
  return fp;
}

// Returns the first instruction of `block` rendered, to identify destinations.
std::string first_string_literal(cfg::Block* block) {
  for (auto& mie : InstructionIterable(block)) {
    if (mie.insn->opcode() == OPCODE_CONST_STRING) {
      return std::string(mie.insn->get_string()->str());
    }
  }
  return "<none>";
}

const DexString* key_string(const StringSwitchInfo::StringKey& k) {
  return std::get<const DexString*>(k);
}

} // namespace

class StringSwitchFinderTest : public RedexTest {};

// A canonical two-stage `switch (String)` over {"one","two","three"}, mirroring
// the d8 desugaring captured from real builds: hashCode -> switch(hash) -> per
// bucket equals()+if-nez setting an ordinal -> switch(ordinal) -> bodies.
TEST_F(StringSwitchFinderTest, hash_switch_three_cases) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
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
      (const v1 666) ; this is to show that v1's exit state is properly used
      (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  // three string cases + default
  EXPECT_EQ(info.key_to_case.size(), 4u);

  std::map<std::string, std::string> string_to_dest;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      string_to_dest["<default>"] = first_string_literal(block);
    } else {
      string_to_dest[std::string(key_string(key)->str())] =
          first_string_literal(block);
    }
  }

  EXPECT_EQ(string_to_dest["one"], "RES_one");
  EXPECT_EQ(string_to_dest["two"], "RES_two");
  EXPECT_EQ(string_to_dest["three"], "RES_three");
  EXPECT_EQ(string_to_dest["<default>"], "RES_default");

  code->clear_cfg();
}

// Like above, but in the equals() chain the subject gets overwritten. This MUST
// be rejected, not a real switch.
TEST_F(StringSwitchFinderTest, hash_switch_subject_overwritten_before_equals) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const-string "omg")
      (move-result-pseudo-object v5)
      (const v1 -1)
      (switch v0 (:hone :htwo :hthree))

      (:ord)
      (switch v1 (:body0 :body1 :body2))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:hone 110182)
      (const-string "one")
      (move-result-pseudo-object v4)
      (move-object v3 v5)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 0u);

  code->clear_cfg();
}

// Like above, but in the equals() chain the subject is simply different. This
// MUST be rejected, not a real switch.
TEST_F(StringSwitchFinderTest, hash_switch_subject_differs) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const-string "omg")
      (move-result-pseudo-object v5)
      (const v1 -1)
      (switch v0 (:hone :htwo :hthree))

      (:ord)
      (switch v1 (:body0 :body1 :body2))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:hone 110182)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v5 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 0u);

  code->clear_cfg();
}

// Like above, but in the equals() chain the result of equals() gets overwritten
// before the branch. This MUST be rejected, not a real switch.
// equal_neq_edges() is responsible for validating this, at the time of writing.
TEST_F(StringSwitchFinderTest, hash_switch_equals_overwritten_before_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
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
      (const v2 999)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 0u);

  code->clear_cfg();
}

// Like the previous, except the const-string for a certain case appears in the
// origin block and gets used later.
TEST_F(StringSwitchFinderTest, hash_switch_three_cases_string_defined_earlier) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const-string "one")
      (move-result-pseudo-object v4)
      (const v1 -1)
      (switch v0 (:hone :htwo :hthree))

      (:ord)
      (switch v1 (:body0 :body1 :body2))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:hone 110182)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  // three string cases + default
  EXPECT_EQ(info.key_to_case.size(), 4u);

  std::map<std::string, std::string> string_to_dest;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      string_to_dest["<default>"] = first_string_literal(block);
    } else {
      string_to_dest[std::string(key_string(key)->str())] =
          first_string_literal(block);
    }
  }

  EXPECT_EQ(string_to_dest["one"], "RES_one");
  EXPECT_EQ(string_to_dest["two"], "RES_two");
  EXPECT_EQ(string_to_dest["three"], "RES_three");
  EXPECT_EQ(string_to_dest["<default>"], "RES_default");

  code->clear_cfg();
}

// A "decoy": structurally identical, but one hash-switch key does not match the
// java hashCode of the literal in that bucket. Must be rejected.
TEST_F(StringSwitchFinderTest, decoy_wrong_hash_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:hone :htwo))

      (:ord)
      (switch v1 (:body0 :body1))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:hone 999)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
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

      (:body0 0)
      (const-string "RES_one")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:body1 1)
      (const-string "RES_two")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// A plain int switch over hashCode() with no equals() checks is not a string
// switch and must be rejected.
TEST_F(StringSwitchFinderTest, raw_hash_switch_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (switch v0 (:a :b))
      (const v1 0)
      (return v1)
      (:a 1)
      (const v1 100)
      (return v1)
      (:b 2)
      (const v1 200)
      (return v1)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// Two independent String switches in one method: find_string_switches should
// return both, with non-overlapping regions.
TEST_F(StringSwitchFinderTest, multiple_switches_in_one_cfg) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v6)
      (load-param-object v3)
      (if-eqz v6 :second)

      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:hA))
      (:ordA)
      (switch v1 (:bodyA))
      (const-string "defA")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:hA 110182)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :setA)
      (goto :ordA)
      (:setA)
      (const v1 0)
      (goto :ordA)
      (:bodyA 0)
      (const-string "RA")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:second)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:hB))
      (:ordB)
      (switch v1 (:bodyB))
      (const-string "defB")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:hB 115276)
      (const-string "two")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :setB)
      (goto :ordB)
      (:setB)
      (const v1 0)
      (goto :ordB)
      (:bodyB 0)
      (const-string "RB")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 2u);

  std::set<std::string> recovered;
  for (const auto& info : switches) {
    EXPECT_EQ(info.key_to_case.size(), 2u); // one string case + default
    for (const auto& [key, block] : info.key_to_case) {
      if (!std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
        recovered.insert(std::string(key_string(key)->str()));
      }
    }
  }
  EXPECT_EQ(recovered, (std::set<std::string>{"one", "two"}));
  // The two regions must be disjoint.
  EXPECT_FALSE(switches[0].region_blocks.empty());
  for (auto* b : UnorderedIterable(switches[0].region_blocks)) {
    EXPECT_EQ(switches[1].region_blocks.count(b), 0u);
  }
  code->clear_cfg();
}

// Two hashCode() calls (on different paths) feed a single switch. Both anchor
// the same switch, so exactly one result should be reported (the duplicate is
// de-duplicated by region overlap).
TEST_F(StringSwitchFinderTest, overlapping_anchors_deduplicated) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v6)
      (load-param-object v3)
      (if-eqz v6 :elsehc)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (goto :afterhc)
      (:elsehc)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (:afterhc)
      (const v1 -1)
      (switch v0 (:hone))
      (:ord)
      (switch v1 (:body0))
      (const-string "def")
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
      (goto :ord)
      (:body0 0)
      (const-string "R1")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_EQ(find_string_switches(cfg, fp).size(), 1u);
  code->clear_cfg();
}

// An external block jumps into a block that is part of the switching machinery
// (the ordinal-setting block). That makes a region block reachable from outside
// R, so the switch must be rejected (it is otherwise a valid 1-case switch).
TEST_F(StringSwitchFinderTest, external_pred_into_region_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (load-param v6)
      (if-eqz v6 :sneak)

      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:hone))
      (:ord)
      (switch v1 (:body0))
      (const-string "def")
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
      (goto :ord)
      (:sneak)
      (goto :set0)
      (:body0 0)
      (const-string "R1")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// Form B: the linear equals-chain shape d8 emits for small switches -- a
// (discarded) hashCode null-guard followed by `if (s.equals(lit))` checks
// branching directly to bodies. Mirrors AnotherExample.lookup.
TEST_F(StringSwitchFinderTest, equals_chain_two_cases) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
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
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::EQUALS_CHAIN);
  EXPECT_EQ(info.key_to_case.size(), 3u);
  EXPECT_EQ(info.chain_order,
            (std::vector<const DexString*>{DexString::make_string("abc"),
                                           DexString::make_string("xyz")}));

  std::map<std::string, std::string> string_to_dest;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      string_to_dest["<default>"] = first_string_literal(block);
    } else {
      string_to_dest[std::string(key_string(key)->str())] =
          first_string_literal(block);
    }
  }
  EXPECT_EQ(string_to_dest["abc"], "RES_first");
  EXPECT_EQ(string_to_dest["xyz"], "RES_second");
  EXPECT_EQ(string_to_dest["<default>"], "RES_default");
  code->clear_cfg();
}

// Form B where two literals share a body (`case "a": case "b": ...`). Distinct
// literals mapping to one destination is valid (unlike the ordinal bijection).
TEST_F(StringSwitchFinderTest, equals_chain_shared_body) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "a")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :shared)
      (const-string "b")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :shared)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:shared)
      (const-string "RES_shared")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::EQUALS_CHAIN);
  EXPECT_EQ(info.key_to_case.size(), 3u);
  cfg::Block* a_dest = info.key_to_case.at(
      StringSwitchInfo::StringKey(DexString::get_string("a")));
  cfg::Block* b_dest = info.key_to_case.at(
      StringSwitchInfo::StringKey(DexString::get_string("b")));
  EXPECT_EQ(a_dest, b_dest); // both share the same body
  EXPECT_EQ(first_string_literal(a_dest), "RES_shared");
  code->clear_cfg();
}

// Form B but the case bodies fall through / merge back into the chain (an
// accumulating `if (s.equals(...)) {...}` sequence). The bodies are not exits,
// so a chain block ends up with a predecessor outside the region: rejected.
TEST_F(StringSwitchFinderTest, equals_chain_merge_back_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "a")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-eqz v0 :checkb)
      (const v2 1)
      (goto :checkb)
      (:checkb)
      (const-string "b")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-eqz v1 :end)
      (const v2 2)
      (goto :end)
      (:end)
      (return-void)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// A degenerate one-case equals chain where d8 reuses the subject's register for
// the sole equals() result: by the dispatch branch the subject register no
// longer holds the subject. Such a switch can't be transformed into anything
// smaller or faster, and its origin branch would read a clobbered subject, so
// recovery rejects it -- keeping subject_reg usable at origin_insn for every
// transform.
TEST_F(StringSwitchFinderTest,
       one_case_chain_reusing_subject_register_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :first)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// The same one case, but the equals() result goes to a scratch register (not
// the subject's), so the subject survives to the branch and the switch IS
// recovered. Confirms the rejection above keys on the clobber, not merely the
// case count.
TEST_F(StringSwitchFinderTest, one_case_chain_preserving_subject_recovered) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :first)
      (const-string "RES_default")
      (move-result-pseudo-object v3)
      (return-object v3)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v3)
      (return-object v3)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  EXPECT_EQ(switches[0].form, StringSwitchInfo::Form::EQUALS_CHAIN);
  code->clear_cfg();
}

// Form A where d8 preloads a case's ordinal in a dominating block and routes
// the equals-true edge straight to the ordinal switch (no per-case const-set
// block). Mirrors what d8 emitted for Example.handleMultiple.
TEST_F(StringSwitchFinderTest, hash_switch_preloaded_ordinal) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (const v1 0)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (switch v0 (:hone))
      (:hdefault)
      (const v1 -1)
      (goto :ord)
      (:ord)
      (switch v1 (:b0))
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:hone 110182)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :ord)
      (goto :hdefault)
      (:b0 0)
      (const-string "RES_one")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  EXPECT_EQ(info.key_to_case.size(), 2u);
  cfg::Block* one_dest = info.key_to_case.at(
      StringSwitchInfo::StringKey(DexString::get_string("one")));
  EXPECT_EQ(first_string_literal(one_dest), "RES_one");
  code->clear_cfg();
}

// Form A wrapped in a try/catch (single handler). Inside the try every
// may-throw instruction (the hashCode/equals invokes AND the const-string
// literals) ends its own block, so the equals invoke is split from both its
// const-string and its consuming branch. The switch must still be recovered,
// with the handler recorded as the lone Throwable exit. Mirrors
// Example.wrappedInTryCatch.
TEST_F(StringSwitchFinderTest, hash_switch_in_try_single_catch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (.try_start tc)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
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
      (.try_end tc)

      (.catch (tc) "Ljava/lang/Exception;")
      (move-exception v5)
      (const-string "RES_caught")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  EXPECT_EQ(info.key_to_case.size(), 4u);

  std::map<std::string, std::string> string_to_dest;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      string_to_dest["<default>"] = first_string_literal(block);
    } else {
      string_to_dest[std::string(key_string(key)->str())] =
          first_string_literal(block);
    }
  }
  EXPECT_EQ(string_to_dest["one"], "RES_one");
  EXPECT_EQ(string_to_dest["two"], "RES_two");
  EXPECT_EQ(string_to_dest["three"], "RES_three");
  EXPECT_EQ(string_to_dest["<default>"], "RES_default");

  // The single Throwable exit: Exception -> the catch handler.
  ASSERT_EQ(info.catch_exits.size(), 1u);
  EXPECT_EQ(info.catch_exits[0].catch_type,
            DexType::get_type("Ljava/lang/Exception;"));
  EXPECT_EQ(info.catch_exits[0].index, 0u);
  EXPECT_EQ(first_string_literal(info.catch_exits[0].handler), "RES_caught");
  EXPECT_TRUE(info.catch_exits[0].handler->is_catch());
  code->clear_cfg();
}

// Form A wrapped in a try with TWO catch handlers. The Throwable exits must be
// recorded in catch-index order. Mirrors Example.wrappedInTryCatchMulti.
TEST_F(StringSwitchFinderTest, hash_switch_in_try_multi_catch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (.try_start tc0)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:hone))

      (:ord)
      (switch v1 (:body0))
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
      (goto :ord)

      (:body0 0)
      (const-string "RES_one")
      (move-result-pseudo-object v4)
      (return-object v4)
      (.try_end tc0)

      (.catch (tc0 tc1) "Ljava/lang/OutOfMemoryError;")
      (move-exception v5)
      (const-string "RES_oom")
      (move-result-pseudo-object v4)
      (return-object v4)

      (.catch (tc1) "Ljava/lang/Exception;")
      (move-exception v5)
      (const-string "RES_exc")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  EXPECT_EQ(info.key_to_case.size(), 2u);

  // Two Throwable exits, ordered by catch index: OutOfMemoryError, then
  // Exception.
  ASSERT_EQ(info.catch_exits.size(), 2u);
  EXPECT_EQ(info.catch_exits[0].catch_type,
            DexType::get_type("Ljava/lang/OutOfMemoryError;"));
  EXPECT_EQ(info.catch_exits[0].index, 0u);
  EXPECT_EQ(first_string_literal(info.catch_exits[0].handler), "RES_oom");
  EXPECT_EQ(info.catch_exits[1].catch_type,
            DexType::get_type("Ljava/lang/Exception;"));
  EXPECT_EQ(info.catch_exits[1].index, 1u);
  EXPECT_EQ(first_string_literal(info.catch_exits[1].handler), "RES_exc");
  code->clear_cfg();
}

// Form B (equals-chain) wrapped in a try/catch. Verifies the chain is recovered
// across the may-throw block splits and the handler is recorded.
TEST_F(StringSwitchFinderTest, equals_chain_in_try) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (.try_start tc)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :first)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:second)
      (const-string "RES_second")
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
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::EQUALS_CHAIN);
  EXPECT_EQ(info.key_to_case.size(), 3u);

  std::map<std::string, std::string> string_to_dest;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      string_to_dest["<default>"] = first_string_literal(block);
    } else {
      string_to_dest[std::string(key_string(key)->str())] =
          first_string_literal(block);
    }
  }
  EXPECT_EQ(string_to_dest["abc"], "RES_first");
  EXPECT_EQ(string_to_dest["xyz"], "RES_second");
  EXPECT_EQ(string_to_dest["<default>"], "RES_default");

  ASSERT_EQ(info.catch_exits.size(), 1u);
  EXPECT_EQ(info.catch_exits[0].catch_type,
            DexType::get_type("Ljava/lang/Exception;"));
  EXPECT_EQ(first_string_literal(info.catch_exits[0].handler), "RES_caught");
  code->clear_cfg();
}

// Two equals tests of one chain sit in SEPARATE try regions routing to
// DIFFERENT handlers. The region's Throwable exits are not uniform, so it must
// be rejected.
TEST_F(StringSwitchFinderTest, divergent_catches_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (.try_start ta)
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (.try_end ta)
      (if-nez v0 :first)
      (.try_start tb)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (.try_end tb)
      (if-nez v2 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v4)
      (return-object v4)
      (:second)
      (const-string "RES_second")
      (move-result-pseudo-object v4)
      (return-object v4)

      (.catch (ta) "Ljava/lang/RuntimeException;")
      (move-exception v5)
      (const-string "RES_a")
      (move-result-pseudo-object v4)
      (return-object v4)

      (.catch (tb) "Ljava/lang/Exception;")
      (move-exception v5)
      (const-string "RES_b")
      (move-result-pseudo-object v4)
      (return-object v4)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// A case body reuses the matched literal's register, so the const-string value
// (defined in a non-origin region block) escapes the region into a destination
// block. A const-string is NOT haulable -- it can throw, so relocating it
// closer to a use could require installing extra catch handlers -- so this
// escape must cause the switch to be rejected.
TEST_F(StringSwitchFinderTest, const_string_escape_into_body_rejected) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :first)
      (const-string "xyz")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :second)
      (const-string "RES_default")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:first)
      (const-string "RES_first")
      (move-result-pseudo-object v1)
      (return-object v1)
      (:second)
      (return-object v0)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// The cheap pre-filter requires BOTH a String.hashCode() and a String.equals()
// call. A method with both is a candidate.
TEST_F(StringSwitchFinderTest, prefilter_true_when_both_present) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.hashCode:()I")
      (move-result v1)
      (const-string "x")
      (move-result-pseudo-object v2)
      (invoke-virtual (v0 v2) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v3)
      (return v3)
    )
  )");
  code->build_cfg();
  EXPECT_TRUE(may_contain_string_switch(code->cfg()));
  code->clear_cfg();
}

// Only hashCode() (no equals): cannot be a string switch, pre-filter rejects.
TEST_F(StringSwitchFinderTest, prefilter_false_when_only_hashcode) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.hashCode:()I")
      (move-result v1)
      (return v1)
    )
  )");
  code->build_cfg();
  EXPECT_FALSE(may_contain_string_switch(code->cfg()));
  code->clear_cfg();
}

// Neither call: pre-filter rejects.
TEST_F(StringSwitchFinderTest, prefilter_false_when_neither) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (return v0)
    )
  )");
  code->build_cfg();
  EXPECT_FALSE(may_contain_string_switch(code->cfg()));
  code->clear_cfg();
}

// Distilled from a real Form A switch that we currently reject. d8 hoists a
// constant that the case bodies share into the second-stage (ordinal) switch
// block -- here a `const-string ""` (the default arg both bodies pass to a
// getString-style call) loaded into v7. That block is part of the switching
// machinery (the region), so v7 escapes the region into the body blocks. A
// const-string is not haulable (it can throw, so relocating it to each body
// could require installing extra catch handlers), so the escape check rejects
// the whole switch. (Replacing the `const-string ""` with a numeric const that
// the bodies share would be accepted -- numeric literals are haulable.)
TEST_F(StringSwitchFinderTest, ordinal_block_preloads_const_string_for_bodies) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v2)
      (invoke-virtual (v2) "Ljava/lang/String;.hashCode:()I")
      (move-result v3)
      (const v5 -1)
      (switch v3 (:hone :htwo))

      (:ord)
      (const-string "")
      (move-result-pseudo-object v7)
      (switch v5 (:body0 :body1))
      (const-string "RES_default")
      (move-result-pseudo-object v0)
      (return-object v0)

      (:hone 110182)
      (const-string "one")
      (move-result-pseudo-object v1)
      (invoke-virtual (v2 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :set0)
      (goto :ord)
      (:set0)
      (const v5 0)
      (goto :ord)

      (:htwo 115276)
      (const-string "two")
      (move-result-pseudo-object v1)
      (invoke-virtual (v2 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v1)
      (if-nez v1 :set1)
      (goto :ord)
      (:set1)
      (const v5 1)
      (goto :ord)

      (:body0 0)
      (return-object v7)

      (:body1 1)
      (return-object v7)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);
  EXPECT_TRUE(find_string_switches(cfg, fp).empty());
  code->clear_cfg();
}

// Many fall-through case labels sharing one body, mirroring:
//   switch (key) {
//     case "abc": case "def": ... case "yz": return "yay";
//     default: return "nay";
//   }
// javac assigns each label a distinct ordinal (0..8) and the ordinal switch
// routes all of them to the single shared body. The finder must recover all 9
// string keys (10 cases incl. default), with the 9 strings collapsing onto one
// destination block.
TEST_F(StringSwitchFinderTest, hash_switch_shared_destination_block) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (switch v0 (:h0 :h1 :h2 :h3 :h4 :h5 :h6 :h7 :h8))

      (:ord)
      (switch v1 (:o0 :o1 :o2 :o3 :o4 :o5 :o6 :o7 :o8))
      (const-string "nay")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:o0 0)
      (:o1 1)
      (:o2 2)
      (:o3 3)
      (:o4 4)
      (:o5 5)
      (:o6 6)
      (:o7 7)
      (:o8 8)
      (const-string "yay")
      (move-result-pseudo-object v4)
      (return-object v4)

      (:h0 96354)
      (const-string "abc")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s0)
      (goto :ord)
      (:s0)
      (const v1 0)
      (goto :ord)

      (:h1 99333)
      (const-string "def")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s1)
      (goto :ord)
      (:s1)
      (const v1 1)
      (goto :ord)

      (:h2 102312)
      (const-string "ghi")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s2)
      (goto :ord)
      (:s2)
      (const v1 2)
      (goto :ord)

      (:h3 105291)
      (const-string "jkl")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s3)
      (goto :ord)
      (:s3)
      (const v1 3)
      (goto :ord)

      (:h4 108270)
      (const-string "mno")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s4)
      (goto :ord)
      (:s4)
      (const v1 4)
      (goto :ord)

      (:h5 111249)
      (const-string "pqr")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s5)
      (goto :ord)
      (:s5)
      (const v1 5)
      (goto :ord)

      (:h6 114228)
      (const-string "stu")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s6)
      (goto :ord)
      (:s6)
      (const v1 6)
      (goto :ord)

      (:h7 117207)
      (const-string "vwx")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s7)
      (goto :ord)
      (:s7)
      (const v1 7)
      (goto :ord)

      (:h8 3873)
      (const-string "yz")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :s8)
      (goto :ord)
      (:s8)
      (const v1 8)
      (goto :ord)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);
  EXPECT_EQ(info.key_to_case.size(), 10u); // 9 strings + default

  std::set<std::string> string_keys;
  std::set<cfg::Block*> string_dests;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      EXPECT_EQ(first_string_literal(block), "nay");
      continue;
    }
    string_keys.insert(std::string(key_string(key)->str()));
    string_dests.insert(block);
  }
  EXPECT_EQ(string_keys,
            (std::set<std::string>{"abc", "def", "ghi", "jkl", "mno", "pqr",
                                   "stu", "vwx", "yz"}));
  // All nine fall-through labels collapse onto the single shared "yay" body.
  ASSERT_EQ(string_dests.size(), 1u);
  EXPECT_EQ(first_string_literal(*string_dests.begin()), "yay");
  code->clear_cfg();
}

// A register (v10) with SEVERAL divergent region defs (0 in the origin, 777 in
// bucket one, 888 in bucket two, 999 in bucket three) all flowing to a single
// use outside the region (in the default body). No single constant reproduces
// the path-dependent value, so this cannot be hauled and the switch must NOT be
// recovered (a deliberate false negative).
TEST_F(StringSwitchFinderTest, hash_switch_divergent_extra_loads_rejected) {
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
      (invoke-static (v10) "Lfoo;.use:(I)V")
      (return-object v4)

      (:hone 110182)
      (const v10 777)
      (const-string "one")
      (move-result-pseudo-object v4)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v2)
      (if-nez v2 :set0)
      (goto :ord)
      (:set0)
      (const v1 0)
      (goto :ord)

      (:htwo 115276)
      (const v10 888)
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
      (const v10 999)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  EXPECT_EQ(switches.size(), 0u);

  code->clear_cfg();
}

// The representable counterpart (EQUALS_CHAIN): `const v10 777` lives in the
// second chain block (a non-origin region block) and is consumed in the "b"
// body. It is on the root->leaf path to BOTH the "b" body (equal edge) and the
// default (not-equal fall-through), so -- mirroring SwitchEquivFinder's
// accumulation -- it is recorded on both leaves, even though only the "b" body
// reads it (the spare copy on the default path is a dead const a later DCE pass
// removes). The switch IS recovered (no divergence: each leaf sees a consistent
// load set).
TEST_F(StringSwitchFinderTest, equals_chain_single_def_extra_load_recorded) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.hashCode:()I")
      (const-string "a")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :body_a)

      (const v10 777)
      (const-string "b")
      (move-result-pseudo-object v0)
      (invoke-virtual (v1 v0) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-nez v0 :body_b)

      (const-string "RES_default")
      (move-result-pseudo-object v3)
      (return-object v3)

      (:body_a)
      (const-string "RES_a")
      (move-result-pseudo-object v3)
      (return-object v3)

      (:body_b)
      (invoke-static (v10) "Lfoo;.use:(I)V")
      (const-string "RES_b")
      (move-result-pseudo-object v3)
      (return-object v3)
    )
  )");
  code->build_cfg();
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::EQUALS_CHAIN);

  cfg::Block* body_b = nullptr;
  for (const auto& [key, block] : info.key_to_case) {
    if (!std::holds_alternative<StringSwitchInfo::DefaultCase>(key) &&
        key_string(key)->str() == "b") {
      body_b = block;
    }
  }
  ASSERT_NE(body_b, nullptr);
  auto default_block = info.default_case();
  ASSERT_TRUE(default_block.has_value());

  const auto& extra_loads = info.extra_loads;
  // `const v10 777` is on the path to both the "b" body and the default.
  ASSERT_EQ(extra_loads.size(), 2u);
  auto expect_v10_777 = [&](cfg::Block* leaf) {
    auto it = extra_loads.find(leaf);
    ASSERT_TRUE(it != extra_loads.end());
    const auto& loads = it->second; // std::map<reg_t, IRInstruction*>
    ASSERT_EQ(loads.size(), 1u);
    auto load_it = loads.find(10);
    ASSERT_TRUE(load_it != loads.end());
    EXPECT_EQ(load_it->second->opcode(), OPCODE_CONST);
    EXPECT_EQ(load_it->second->get_literal(), 777);
  };
  expect_v10_777(body_b);
  expect_v10_777(*default_block);

  code->clear_cfg();
}

// HASH_SWITCH where `:set0` sets the ordinal (v1=0) AND v10=777 together, and
// the "one" body (the v1==0 arm) consumes v10. v10==777 holds there exactly
// because v1==0 -- the correlation the second-stage ordinal switch encodes. A
// purely structural walk would fan out from the ordinal switch along infeasible
// (path, body) pairs and report spurious divergence; the ordinal-aware walk
// follows only the v1==0 arm, so it proves v10==777 reaches the body and
// records it. (The other bodies and the default see no v10 def on their paths.)
TEST_F(StringSwitchFinderTest, hash_switch_correlated_extra_load_recorded) {
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);

  cfg::Block* body_one = nullptr;
  for (const auto& [key, block] : info.key_to_case) {
    if (!std::holds_alternative<StringSwitchInfo::DefaultCase>(key) &&
        key_string(key)->str() == "one") {
      body_one = block;
    }
  }
  ASSERT_NE(body_one, nullptr);

  // Only the "one" body carries an extra load (v10 = 777); the other bodies and
  // the default never have v10 defined on their paths.
  const auto& extra_loads = info.extra_loads;
  ASSERT_EQ(extra_loads.size(), 1u);
  auto it = extra_loads.find(body_one);
  ASSERT_TRUE(it != extra_loads.end());
  const auto& loads = it->second; // std::map<reg_t, IRInstruction*>
  ASSERT_EQ(loads.size(), 1u);
  auto load_it = loads.find(10);
  ASSERT_TRUE(load_it != loads.end());
  EXPECT_EQ(load_it->second->opcode(), OPCODE_CONST);
  EXPECT_EQ(load_it->second->get_literal(), 777);

  // Materializing the extra loads puts `const v10 777` immediately before the
  // `use(v10)` that consumes it in the "one" body.
  copy_extra_loads_to_leaf_blocks(cfg, info.extra_loads);
  auto* use_method = DexMethod::get_method("Lfoo;.use:(I)V");
  ASSERT_NE(use_method, nullptr);
  IRInstruction* prev = nullptr;
  bool found_use = false;
  for (auto& mie : InstructionIterable(body_one)) {
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

TEST_F(StringSwitchFinderTest,
       hash_switch_correlated_extra_load_recorded_another) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (invoke-virtual (v3) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (const v1 -1)
      (const v10 0)
      (const v11 0)
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
      (move v1 v11)
      ; this would be successfully found (const v1 0)
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
  auto& cfg = code->cfg();
  auto fp = make_fixpoint(cfg);

  auto switches = find_string_switches(cfg, fp);
  ASSERT_EQ(switches.size(), 1u);
  const auto& info = switches[0];
  EXPECT_EQ(info.form, StringSwitchInfo::Form::HASH_SWITCH);

  cfg::Block* body_one = nullptr;
  for (const auto& [key, block] : info.key_to_case) {
    if (!std::holds_alternative<StringSwitchInfo::DefaultCase>(key) &&
        key_string(key)->str() == "one") {
      body_one = block;
    }
  }
  ASSERT_NE(body_one, nullptr);

  // Only the "one" body carries an extra load (v10 = 777); the other bodies and
  // the default never have v10 defined on their paths.
  const auto& extra_loads = info.extra_loads;
  ASSERT_EQ(extra_loads.size(), 1u);
  auto it = extra_loads.find(body_one);
  ASSERT_TRUE(it != extra_loads.end());
  const auto& loads = it->second; // std::map<reg_t, IRInstruction*>
  ASSERT_EQ(loads.size(), 1u);
  auto load_it = loads.find(10);
  ASSERT_TRUE(load_it != loads.end());
  EXPECT_EQ(load_it->second->opcode(), OPCODE_CONST);
  EXPECT_EQ(load_it->second->get_literal(), 777);

  // Materializing the extra loads puts `const v10 777` immediately before the
  // `use(v10)` that consumes it in the "one" body.
  copy_extra_loads_to_leaf_blocks(cfg, info.extra_loads);
  auto* use_method = DexMethod::get_method("Lfoo;.use:(I)V");
  ASSERT_NE(use_method, nullptr);
  IRInstruction* prev = nullptr;
  bool found_use = false;
  for (auto& mie : InstructionIterable(body_one)) {
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
