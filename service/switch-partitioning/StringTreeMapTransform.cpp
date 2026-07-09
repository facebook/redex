/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringTreeMapTransform.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "ControlFlow.h"
#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "SourceBlocks.h"
#include "StringSwitchFinder.h"
#include "StringTreeSet.h"

namespace {

// Ordinal 0 is reserved by the lookup method to mean "not found" (-> default).
// Ordinals are encoded with int32_t: the lookup method returns a Java int and
// the trie decoder reconstructs the value from a header-declared count of 6-bit
// payload units, so int16_t and int32_t share the SAME lookup method and (for
// our always-positive ordinals) produce byte-identical payloads -- int32_t just
// lifts the value ceiling from 32767 to ~2^31.
constexpr int32_t STRING_TREE_NO_ENTRY = 0;
// ~2 code units (4 bytes) is a rough average dex instruction size, used only to
// weigh "instructions removed" against "instructions added" in the heuristic
// score. The trie payload's byte cost is counted exactly.
constexpr int64_t APPROX_INSN_BYTES = 4;

// The trie encoder requires every key char be >= 32, and a clean MUTF-8 <->
// UTF-16 round-trip requires ASCII; reject anything outside printable ASCII.
bool is_ascii_key(const DexString* s) {
  for (unsigned char c : s->str()) {
    if (c < 32 || c > 127) {
      return false;
    }
  }
  return true;
}

// Orders blocks by id(), matching cfg::Block::operator<. Used to assign
// sequential ordinals so the emitted switch is packed.
struct block_id_less {
  bool operator()(const cfg::Block* a, const cfg::Block* b) const {
    return a->id() < b->id();
  }
};

// The concrete re-encoding of a recovered switch: the case-label -> ordinal map
// (already encoded into `encoded`), the switch's (ordinal -> distinct
// destination) edges, and the default block.
struct TreeMapPlan : TransformPlan {
  std::vector<std::pair<int32_t, cfg::Block*>> edges;
  cfg::Block* default_block{nullptr};
  std::string encoded;
};

// Builds the plan, or std::nullopt if the switch cannot be re-encoded: a
// non-ASCII case label or a missing default. Deterministic, so evaluate() and
// apply() agree.
std::optional<TreeMapPlan> build_plan(const StringSwitchInfo& info) {
  auto default_opt = info.default_case();
  if (!default_opt || *default_opt == nullptr) {
    return std::nullopt;
  }

  // Distinct destinations among the string cases, in block order. Distinct
  // labels may share a body (e.g. `case "a": case "b":`); shared bodies get one
  // ordinal and one switch edge.
  std::set<cfg::Block*, block_id_less> distinct_dests;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      continue;
    }
    if (block == nullptr) {
      return std::nullopt;
    }
    if (!is_ascii_key(std::get<const DexString*>(key))) {
      return std::nullopt;
    }
    distinct_dests.insert(block);
  }
  if (distinct_dests.empty()) {
    return std::nullopt;
  }

  TreeMapPlan plan;
  plan.default_block = *default_opt;
  // Lookup-only (never iterated), so an unordered map is fine here. The encoded
  // payload's determinism comes from `distinct_dests` (block order) and `items`
  // (a std::map), not from this.
  UnorderedMap<cfg::Block*, int32_t> dest_to_ordinal;
  int32_t counter = STRING_TREE_NO_ENTRY + 1;
  for (auto* block : distinct_dests) {
    int32_t ordinal = counter++;
    dest_to_ordinal.emplace(block, ordinal);
    plan.edges.emplace_back(ordinal, block);
  }

  std::map<std::string, int32_t> items;
  for (const auto& [key, block] : info.key_to_case) {
    if (std::holds_alternative<StringSwitchInfo::DefaultCase>(key)) {
      continue;
    }
    items.emplace(std::string(std::get<const DexString*>(key)->str()),
                  dest_to_ordinal.at(block));
  }
  plan.encoded = StringTreeMap<int32_t>::encode_string_tree_map(items);
  return plan;
}

} // namespace

std::optional<TransformScore> StringTreeMapTransform::evaluate(
    const StringSwitchCandidate& candidate) const {
  const auto& info = candidate.info;

  // Cold origin only. A hot dispatch is left for a (future) PERFORMANCE-tier
  // transform; re-encoding it would trade speed for size on a hot path.
  if (source_blocks::is_hot(info.origin_block)) {
    return std::nullopt;
  }
  // Region constants consumed in a body (info.extra_loads) are handled: apply()
  // copies them into the bodies before excising the region. The finder only
  // reports representable extra_loads (it bails on divergence), so there is
  // nothing unsafe to accept here.
  // Case count, including the default (matches the analysis report's CASES).
  if (static_cast<int64_t>(info.key_to_case.size()) < m_min_cases) {
    return std::nullopt;
  }
  auto plan = build_plan(info);
  if (!plan) {
    return std::nullopt;
  }
  // V1: only inline const-string payloads (no building a large string in
  // <clinit> yet).
  if (plan->encoded.size() > m_max_payload_size) {
    return std::nullopt;
  }

  // Estimated dex bytes saved: machinery removed, minus the prologue (~5
  // instructions) and the packed switch (one case per distinct destination),
  // minus the trie payload stored in the string pool.
  size_t removed_insns = 0;
  for (auto* b : UnorderedIterable(info.region_blocks)) {
    removed_insns += b->num_opcodes();
  }
  int64_t added_insns = 5 + static_cast<int64_t>(plan->edges.size());
  int64_t magnitude =
      (static_cast<int64_t>(removed_insns) - added_insns) * APPROX_INSN_BYTES -
      static_cast<int64_t>(plan->encoded.size());
  return TransformScore{TransformTier::SIZE, magnitude,
                        std::make_unique<TreeMapPlan>(std::move(*plan))};
}

size_t StringTreeMapTransform::apply(const StringSwitchCandidate& candidate,
                                     const TransformPlan* plan_base) const {
  auto& cfg = candidate.ctx.cfg();
  const auto& info = candidate.info;
  // evaluate() produced this plan for this candidate; reuse it verbatim.
  always_assert(plan_base != nullptr);
  const auto* plan = dynamic_cast<const TreeMapPlan*>(plan_base);
  always_assert(plan != nullptr && plan->encoded.size() <= m_max_payload_size);

  // Make each body self-contained before excising the region: clone any
  // escaping region constant to the front of the leaf that consumes it. The
  // originals die with the region below; the clones keep those bodies correct.
  copy_extra_loads_to_leaf_blocks(cfg, info.extra_loads);

  // The lookup's instructions. `subject_reg` is the hashCode receiver; it is
  // defined before the dispatch branch and never reassigned by the (recovered)
  // machinery, so it is live here.
  auto payload_reg = cfg.allocate_temp();
  auto notfound_reg = cfg.allocate_temp();
  auto result_reg = cfg.allocate_temp();

  auto* const_string = (new IRInstruction(OPCODE_CONST_STRING))
                           ->set_string(DexString::make_string(plan->encoded));
  auto* payload_move = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                           ->set_dest(payload_reg);
  auto* const_notfound = new IRInstruction(OPCODE_CONST);
  const_notfound->set_literal(STRING_TREE_NO_ENTRY);
  const_notfound->set_dest(notfound_reg);
  auto* invoke = (new IRInstruction(OPCODE_INVOKE_STATIC))
                     ->set_method(m_lookup_method)
                     ->set_srcs_size(3);
  invoke->set_src(0, info.subject_reg);
  invoke->set_src(1, payload_reg);
  invoke->set_src(2, notfound_reg);
  auto* result_move =
      (new IRInstruction(OPCODE_MOVE_RESULT))->set_dest(result_reg);
  auto* new_switch = (new IRInstruction(OPCODE_SWITCH))->set_src(0, result_reg);

  // The dispatch's terminating branch -- the first-stage hash switch
  // (HASH_SWITCH) or the first equals-result `if` (EQUALS_CHAIN). Everything
  // before it (incl. any unrelated code between the hashCode invoke and the
  // branch) is preserved; the now-dead hashCode/equals invokes are removed by
  // LocalDce below.
  auto origin_it = cfg.find_insn(info.origin_insn);
  cfg::Block* block = origin_it.block();

  if (info.catch_exits.empty()) {
    // Outside a try nothing throws into a handler, so the whole lookup and the
    // switch can share `block`. replace_insns drops the branch's case edges
    // (leaving its goto) and inserts the linear lookup; create_branch appends
    // the switch, retargets that goto to the default block (where the lookup's
    // "not found" ordinal 0 lands), and adds one case edge per destination.
    cfg.replace_insns(origin_it, {const_string, payload_move, const_notfound,
                                  invoke, result_move});
    cfg.create_branch(block, new_switch, plan->default_block, plan->edges);
  } else {
    // Inside a try, every throwing instruction must end its block, and a switch
    // must not share a block with throwing instructions. Mirror what the
    // compiler emits, spreading the lookup across three blocks:
    //   block: [...preceding...; const-string]            (throws)
    //   mid:   [move-result-pseudo; const 0; invoke]       (throws)
    //   tail:  [move-result; switch]                       (no throw)
    cfg.remove_insn(origin_it); // drop the branch, keep its goto edge
    block->push_back(const_string);

    auto* mid = cfg.create_block();
    mid->push_back(payload_move);
    mid->push_back(const_notfound);
    mid->push_back(invoke);

    auto* tail = cfg.create_block();
    tail->push_back(result_move);
    cfg.create_branch(tail, new_switch, plan->default_block, plan->edges);

    // block --goto--> mid --goto--> tail; reuse the branch's surviving goto for
    // block -> mid.
    auto* goto_edge = cfg.get_succ_edge_of_type(block, cfg::EDGE_GOTO);
    always_assert(goto_edge != nullptr);
    cfg.set_edge_target(goto_edge, mid);
    cfg.add_edge(mid, tail, cfg::EDGE_GOTO);

    // Re-attach the recovered handlers to the two throwing blocks. The finder
    // validated `block` shares exactly `catch_exits`, so first drop any throw
    // edges it already carries, then set both blocks to exactly that set.
    cfg.delete_succ_edge_if(
        block, [](const cfg::Edge* e) { return e->type() == cfg::EDGE_THROW; });
    for (auto* throwing : {block, mid}) {
      for (const auto& ce : info.catch_exits) {
        cfg.add_edge(throwing, ce.handler, const_cast<DexType*>(ce.catch_type),
                     ce.index);
      }
    }
  }

  // The downstream machinery is now unreachable; prune it. The dispatch's
  // now-dead pure invocations (String.hashCode/equals) and the constants that
  // fed them remain reachable in the origin block; the driver clears them with
  // a single LocalDce once all transforms on this method are done.
  cfg.remove_unreachable_blocks();

  // Terminal: the switch is now a lookup, no longer a recoverable string
  // switch.
  return 1;
}
