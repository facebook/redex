/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringSwitchFinder.h"

#include <map>
#include <optional>
#include <variant>
#include <vector>

#include "BigBlocks.h"
#include "ConstantEnvironment.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "LiveRange.h"
#include "MethodUtil.h"
#include "SwitchEquivFinder.h"

// This finder implements two forms:
//   - Form A (HASH_SWITCH): the two-stage hashCode->switch->equals->ordinal->
//     switch shape.
//   - Form B (EQUALS_CHAIN): a linear `if (equals(subject, lit))` chain
//     anchored by a (discarded) hashCode null-guard.
// Both forms are supported inside a try/catch. Exception control flow changes
// the block structure (every may-throw instruction -- the hashCode/equals
// invokes -- must end its block inside a try, splitting the invoke from its
// consuming branch); a BigBlock (a run of blocks split only by throw edges)
// spans those pieces, so each dispatch link is recovered via
// find_equals_link(). The throw edges to the enclosing handlers are recorded as
// the region's Throwable exits (valid_catch_exits) for faithful re-attachment
// when the region is rewritten.

namespace {

namespace cp = constant_propagation;
using namespace live_range;

// Tracks blocks already visited while walking the CFG so a traversal can guard
// against revisiting one (e.g. a cycle). `visit(b)` returns true the first time
// `b` is seen; it returns false thereafter, and also for a null block, so it
// reads naturally as a complete loop guard:
//   CycleGuard guard;
//   while (guard.visit(b)) { ...; b = next; }
class CycleGuard {
 public:
  bool visit(cfg::Block* b) { return b != nullptr && m_seen.insert(b).second; }

 private:
  UnorderedSet<cfg::Block*> m_seen;
};

bool is_string_hashcode(const IRInstruction* insn) {
  return insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
         insn->get_method() == method::java_lang_String_hashCode();
}

bool is_string_equals(const IRInstruction* insn) {
  return insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
         insn->get_method() == method::java_lang_String_equals();
}

bool block_ends_with_switch(cfg::Block* b) {
  auto last = b->get_last_insn();
  return last != b->end() && last->insn->opcode() == OPCODE_SWITCH;
}

sparta::PatriciaTreeSet<Def> defs_of(const UseDefChains& ud,
                                     IRInstruction* insn,
                                     src_index_t idx) {
  auto it = ud.find(Use{insn, idx});
  if (it != ud.end()) {
    return it->second;
  }
  return {};
}

std::optional<int32_t> get_const_int(const ConstantEnvironment& env,
                                     reg_t reg) {
  auto scd = env.get(reg).maybe_get<SignedConstantDomain>();
  if (scd && scd->get_constant()) {
    return static_cast<int32_t>(*scd->get_constant());
  }
  return std::nullopt;
}

const DexString* get_const_string(const ConstantEnvironment& env, reg_t reg) {
  auto sd = env.get(reg).maybe_get<StringDomain>();
  if (sd && sd->get_constant()) {
    return *sd->get_constant();
  }
  return nullptr;
}

// Only these instructions are permitted inside the switching machinery (the
// non-leaf region), analogous to SwitchEquivFinder's is_valid_load_for_nonleaf
// but specialized to a string switch. Anything else means there is interleaved
// logic we cannot safely excise/transform.
bool is_permissible_region_insn(const IRInstruction* insn) {
  auto op = insn->opcode();
  if (opcode::is_a_literal_const(op) || op == OPCODE_CONST_STRING ||
      opcode::is_a_move(op) || opcode::is_move_result_any(op) ||
      op == OPCODE_SWITCH || opcode::is_a_conditional_branch(op)) {
    return true;
  }
  return is_string_hashcode(insn) || is_string_equals(insn);
}

// Given a block ending in a conditional branch that tests the result of
// `equals_insn`, return the "equal" (result != 0) and "not-equal" successor
// edges, normalizing over branch polarity. Uses use-def chains to soundly
// confirm the branch actually tests this equals result (move-aware reaching
// defs attribute the boolean to the equals invoke).
bool equal_neq_edges(cfg::ControlFlowGraph& cfg,
                     const UseDefChains& ud,
                     cfg::Block* block,
                     IRInstruction* equals_insn,
                     cfg::Edge** equal_e,
                     cfg::Edge** neq_e) {
  auto last_it = block->get_last_insn();
  if (last_it == block->end()) {
    return false;
  }
  auto* last = last_it->insn;
  auto op = last->opcode();
  if (!opcode::is_a_conditional_branch(op) || last->srcs_size() != 1) {
    return false;
  }
  auto branch_defs = defs_of(ud, last, 0);
  if (branch_defs.size() != 1 || !branch_defs.contains(equals_insn)) {
    return false; // the branch does not test this equals's result
  }
  auto* branch = cfg.get_succ_edge_of_type(block, cfg::EDGE_BRANCH);
  auto* fall = cfg.get_succ_edge_of_type(block, cfg::EDGE_GOTO);
  if (branch == nullptr || fall == nullptr) {
    return false;
  }
  if (op == OPCODE_IF_NEZ) {
    *equal_e = branch;
    *neq_e = fall;
  } else if (op == OPCODE_IF_EQZ) {
    *equal_e = fall;
    *neq_e = branch;
  } else {
    return false;
  }
  return true;
}

// Convert a block's throw edges (already sorted by catch index) into ordered
// CatchExits mirroring their ThrowInfo.
std::vector<StringSwitchInfo::CatchExit> to_catch_exits(
    const std::vector<cfg::Edge*>& throws) {
  std::vector<StringSwitchInfo::CatchExit> out;
  out.reserve(throws.size());
  for (auto* e : throws) {
    auto* ti = e->throw_info();
    out.push_back({ti->catch_type, ti->index, e->target()});
  }
  return out;
}

bool catch_exits_equal(const std::vector<StringSwitchInfo::CatchExit>& a,
                       const std::vector<StringSwitchInfo::CatchExit>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].catch_type != b[i].catch_type || a[i].index != b[i].index ||
        a[i].handler != b[i].handler) {
      return false;
    }
  }
  return true;
}

// The escaping consts a single recovered runtime `path` carries: scanning its
// blocks in execution order, a later const def shadows an earlier one for the
// same register. The origin block needs no special-casing -- its consts are
// never in `escaping` (escape accounting skips the origin, which stays intact).
SwitchEquivFinder::InstructionSet loads_along_path(
    const std::vector<cfg::Block*>& path,
    const UnorderedSet<IRInstruction*>& escaping) {
  SwitchEquivFinder::InstructionSet loads;
  for (cfg::Block* b : path) {
    for (auto& mie : InstructionIterable(b)) {
      if (escaping.count(mie.insn) != 0u) {
        loads[mie.insn->dest()] = mie.insn;
      }
    }
  }
  return loads;
}

} // namespace

bool StringSwitchInfo::key_comparator::operator()(const StringKey& l,
                                                  const StringKey& r) const {
  bool l_default = std::holds_alternative<DefaultCase>(l);
  bool r_default = std::holds_alternative<DefaultCase>(r);
  if (l_default || r_default) {
    // DefaultCase orders before any string; two defaults are equal.
    if (l_default && r_default) {
      return false;
    }
    return l_default;
  }
  return compare_dexstrings(std::get<const DexString*>(l),
                            std::get<const DexString*>(r));
}

std::optional<cfg::Block*> StringSwitchInfo::default_case() const {
  auto it = key_to_case.find(StringKey(DefaultCase{}));
  if (it == key_to_case.end()) {
    return std::nullopt;
  }
  return it->second;
}

StringSwitchCfgContext::StringSwitchCfgContext(
    cfg::ControlFlowGraph& cfg,
    std::shared_ptr<cp::intraprocedural::FixpointIterator> fixpoint)
    : m_cfg(cfg), m_fixpoint(std::move(fixpoint)) {
  MoveAwareChains chains(m_cfg);
  m_use_def = chains.get_use_def_chains();
  m_def_use = chains.get_def_use_chains();
  for (auto* b : m_cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      m_insn_to_block[mie.insn] = b;
    }
  }
}

cfg::Block* StringSwitchCfgContext::block_of(IRInstruction* insn) const {
  auto it = m_insn_to_block.find(insn);
  return it == m_insn_to_block.end() ? nullptr : it->second;
}

StringSwitchFinder::StringSwitchFinder(
    const StringSwitchCfgContext& ctx,
    const cfg::InstructionIterator& hashcode_insn)
    : m_ctx(ctx), m_hashcode_insn(hashcode_insn) {
  m_success = analyze();
  if (!m_success) {
    m_info = StringSwitchInfo{};
  }
}

bool StringSwitchFinder::ordinal_from_equal_path(
    cfg::Edge* equal_edge,
    cfg::Block* ord_switch_block,
    reg_t ordinal_reg,
    std::vector<cfg::Block*>* ordinal_set_blocks,
    int32_t* out) const {
  // From the equal edge, the ordinal-setting plumbing flows (as a BigBlock)
  // into the ordinal switch. The ordinal arriving at the switch is
  // `ordinal_reg` at the exit of the block right before it. If the equal edge
  // targets the ordinal switch directly (the ordinal was preloaded upstream),
  // that block is the branch (equal_edge->src()); otherwise it is the big
  // block's tail.
  cfg::Block* before_switch;
  if (equal_edge->target() == ord_switch_block) {
    before_switch = equal_edge->src();
  } else {
    auto big_block = big_blocks::get_big_block(equal_edge->target());
    if (!big_block ||
        big_block->get_last_block()->goes_to() != ord_switch_block) {
      return false; // not straight-line plumbing into the ordinal switch
    }
    for (cfg::Block* b : big_block->get_blocks()) {
      ordinal_set_blocks->push_back(b);
    }
    before_switch = big_block->get_last_block();
  }
  const auto& env = m_ctx.fixpoint()->get_exit_state_at(before_switch);
  auto k = get_const_int(env, ordinal_reg);
  if (!k) {
    return false;
  }
  *out = *k;
  return true;
}

StringSwitchFinder::EqualsLink StringSwitchFinder::find_equals_link(
    cfg::Block* start,
    const sparta::PatriciaTreeSet<live_range::Def>& subject_defs) const {
  const auto& ud = m_ctx.use_def();
  // The dispatch's "happy path" from `start` is exactly a BigBlock: a run of
  // blocks that would be one block were they not split by throw edges (inside a
  // try the may-throw const-string literal and equals invoke each end their own
  // block). The equals test, when present, is the first subject.equals(lit) in
  // it; its consuming branch terminates the big block (its last block).
  EqualsLink link;
  // BigBlock has a const member, so it is not assignable; move-construct it
  // into the optional via emplace().
  auto big_block = big_blocks::get_big_block(start);
  if (!big_block) {
    return link;
  }
  link.big_block.emplace(std::move(*big_block));
  for (const auto& mie : big_blocks::InstructionIterable(*link.big_block)) {
    // "Strict value identity": the equals receiver's reaching defs must equal
    // `subject_defs` exactly (not merely overlap). `subject_defs` is non-empty
    // (the caller validates the hashCode receiver's defs), so set equality
    // alone is the right test.
    if (is_string_equals(mie.insn) &&
        subject_defs.equals(defs_of(ud, mie.insn, 0))) {
      link.equals_insn = mie.insn;
      break;
    }
  }
  if (link.equals_insn == nullptr) {
    return link;
  }
  // Read the constant String compared at the equals call from the StringDomain
  // just before it (robust to moves/CSE): replay from the block's entry state
  // up to the call.
  cfg::Block* equals_block = m_ctx.block_of(link.equals_insn);
  auto env = m_ctx.fixpoint()->get_entry_state_at(equals_block);
  for (auto& mie : InstructionIterable(equals_block)) {
    if (mie.insn == link.equals_insn) {
      break;
    }
    m_ctx.fixpoint()->analyze_instruction(mie.insn, &env, /*is_last=*/false);
  }
  link.literal = get_const_string(env, link.equals_insn->src(1));
  return link;
}

bool StringSwitchFinder::valid_catch_exits(
    const UnorderedSet<cfg::Block*>& region, cfg::Block* hashcode_block) {
  // The canonical handler set H: every block that can throw must route to the
  // SAME ordered set of (catch_type, index, handler) edges. Divergent or
  // intermittent throw edges mean the region is not uniformly protected by a
  // single try, so a rewrite could not reproduce its exception behavior --
  // reject (a false negative).
  std::optional<std::vector<StringSwitchInfo::CatchExit>> h;
  auto check = [&](cfg::Block* b) -> bool {
    if (b->cannot_throw()) {
      return true; // contributes no exceptional behavior
    }
    auto exits = to_catch_exits(b->get_outgoing_throws_in_order());
    if (!h) {
      h = std::move(exits);
      return true;
    }
    return catch_exits_equal(*h, exits);
  };

  for (auto* b : UnorderedIterable(region)) {
    if (!check(b)) {
      return false;
    }
  }
  // The hashCode invoke is part of the dispatch (its block sits outside R for
  // Form A inside a try); it must agree with H so the rewrite can reattach
  // throws uniformly.
  if (hashcode_block != nullptr && !check(hashcode_block)) {
    return false;
  }

  if (h && !h->empty()) {
    // A handler must be a clean exit, never itself inside R.
    for (const auto& ce : *h) {
      if (region.count(ce.handler) != 0u) {
        return false;
      }
    }
    m_info.catch_exits = std::move(*h);
  }
  return true;
}

bool StringSwitchFinder::analyze() {
  const auto& ud = m_ctx.use_def();
  auto* hashcode_insn = m_hashcode_insn->insn;
  if (!is_string_hashcode(hashcode_insn)) {
    return false;
  }
  cfg::Block* hc_block = m_ctx.block_of(hashcode_insn);
  if (hc_block == nullptr) {
    return false;
  }

  reg_t subject_reg = hashcode_insn->src(0);
  auto subject_defs = defs_of(ud, hashcode_insn, 0);
  if (subject_defs.empty()) {
    return false;
  }

  // Form A: the hashCode value (move-aware: attributed to the invoke itself,
  // since the move-result is transparent) is consumed by a switch. That
  // switch's block (`b`) is the origin.
  for (auto* b : m_ctx.cfg().blocks()) {
    auto last = b->get_last_insn();
    if (last == b->end() || last->insn->opcode() != OPCODE_SWITCH) {
      continue;
    }
    if (defs_of(ud, last->insn, 0).contains(hashcode_insn)) {
      return decode_hash_switch(subject_reg, subject_defs, b, last->insn);
    }
  }

  // Form B: no consuming switch => the hashCode is a (discarded) null-guard in
  // front of a linear equals-chain, so its block is the origin.
  return decode_equals_chain(subject_reg, subject_defs, hc_block);
}

bool StringSwitchFinder::decode_hash_switch(
    reg_t subject_reg,
    const sparta::PatriciaTreeSet<live_range::Def>& subject_defs,
    cfg::Block* origin_block,
    IRInstruction* hash_switch_insn) {
  auto& cfg = m_ctx.cfg();
  const auto& ud = m_ctx.use_def();
  auto* hashcode_insn = m_hashcode_insn->insn;
  reg_t hash_reg = hash_switch_insn->src(0);

  // Recover the hash dispatch via SwitchEquivFinder.
  SwitchEquivFinder hash_sef(&cfg, cfg.find_insn(hash_switch_insn), hash_reg,
                             SwitchEquivFinder::NO_LEAF_DUPLICATION,
                             m_ctx.fixpoint(),
                             SwitchEquivFinder::EXECUTION_ORDER);
  if (!hash_sef.success() ||
      !hash_sef.are_keys_uniform(SwitchEquivFinder::KeyKind::INT) ||
      !hash_sef.default_case()) {
    return false;
  }

  // The ordinal switch is reached by following the hash switch's default
  // (no-match) path: a BigBlock of plumbing that flows into the (multi-pred)
  // ordinal switch. The switch is the big block's tail if it ends there, else
  // the join the big block flows into.
  auto default_big_block = big_blocks::get_big_block(*hash_sef.default_case());
  if (!default_big_block) {
    return false;
  }
  cfg::Block* ord_switch_block = default_big_block->get_last_block();
  if (!block_ends_with_switch(ord_switch_block)) {
    ord_switch_block = ord_switch_block->goes_to();
  }
  if (ord_switch_block == nullptr ||
      !block_ends_with_switch(ord_switch_block)) {
    return false;
  }
  auto* ord_switch_insn = ord_switch_block->get_last_insn()->insn;
  reg_t ordinal_reg = ord_switch_insn->src(0);

  SwitchEquivFinder ord_sef(&cfg, cfg.find_insn(ord_switch_insn), ordinal_reg,
                            SwitchEquivFinder::NO_LEAF_DUPLICATION,
                            m_ctx.fixpoint(),
                            SwitchEquivFinder::EXECUTION_ORDER);
  if (!ord_sef.success() ||
      !ord_sef.are_keys_uniform(SwitchEquivFinder::KeyKind::INT) ||
      !ord_sef.default_case()) {
    return false;
  }

  cfg::Block* default_body = *ord_sef.default_case();

  // Capture, per leaf, the ordered region blocks a real execution traverses to
  // reach it (origin excluded; it stays intact). The string bodies and the
  // default each get one path per dispatch route; extra-loads accounting later
  // replays these instead of re-deriving the ordinal selector.
  LeafPaths leaf_paths;

  // The hash switch's default arm: origin -> default plumbing -> ordinal switch
  // -> (no ordinal match) -> default body.
  {
    std::vector<cfg::Block*> path;
    for (cfg::Block* b : default_big_block->get_blocks()) {
      if (b != ord_switch_block) {
        path.push_back(b);
      }
    }
    path.push_back(ord_switch_block);
    leaf_paths[default_body].push_back(std::move(path));
  }

  // Decode each hash bucket's equals chain into string -> ordinal -> dest.
  StringSwitchInfo::KeyToCase key_to_case;
  UnorderedSet<const DexString*> seen_literals;
  UnorderedSet<int32_t> seen_ordinals;
  for (const auto& [key, bucket_entry] : hash_sef.key_to_case()) {
    if (SwitchEquivFinder::is_default_case(key)) {
      continue;
    }
    int32_t hash_key = boost::get<int32_t>(key);

    cfg::Block* cur = bucket_entry;
    bool first = true;
    // The not-equal chain blocks visited so far in THIS bucket: every later
    // link (and the bucket's default exit) is reached only after these tests
    // fail.
    std::vector<cfg::Block*> prefix;
    // The collision chain advances via not-equal edges (not the happy path).
    CycleGuard guard;
    while (guard.visit(cur)) {
      // Locate this collision-chain link's equals test. Inside a try the equals
      // is a goto hop or two past `cur` (may-throw const-string/invoke splits),
      // so the BigBlock from `cur` spans the literal, the equals, and the
      // branch.
      EqualsLink link = find_equals_link(cur, subject_defs);
      if (!link.found()) {
        if (first) {
          return false; // a bucket must contain at least one equals
        }
        // Collision chain exhausted: the not-equal path's plumbing runs to the
        // ordinal switch (a multi-pred join the big block stops before), then
        // to the default body. Record the path.
        auto exhaustion_blocks = link.path_blocks(/*until=*/ord_switch_block);
        std::vector<cfg::Block*> path = prefix;
        path.insert(path.end(), exhaustion_blocks.begin(),
                    exhaustion_blocks.end());
        path.push_back(ord_switch_block);
        leaf_paths[default_body].push_back(std::move(path));
        break;
      }
      first = false;

      if (link.literal->java_hashcode() != hash_key) {
        return false; // decoy / mismatched hash bucket
      }

      cfg::Edge* equal_e = nullptr;
      cfg::Edge* neq_e = nullptr;
      if (!equal_neq_edges(cfg, ud, link.branch_block(), link.equals_insn,
                           &equal_e, &neq_e)) {
        return false;
      }
      always_assert(equal_e != nullptr && neq_e != nullptr);
      int32_t ordinal;
      std::vector<cfg::Block*> ordinal_set_blocks;
      if (!ordinal_from_equal_path(equal_e, ord_switch_block, ordinal_reg,
                                   &ordinal_set_blocks, &ordinal)) {
        return false;
      }
      auto dest_it =
          ord_sef.key_to_case().find(SwitchEquivFinder::SwitchingKey(ordinal));
      if (dest_it == ord_sef.key_to_case().end()) {
        return false;
      }
      if (!seen_literals.insert(link.literal).second ||
          !seen_ordinals.insert(ordinal).second) {
        // Duplicate literal or two literals mapping to one ordinal: not a clean
        // bijection.
        return false;
      }
      key_to_case[StringSwitchInfo::StringKey(link.literal)] = dest_it->second;

      // This matching link's blocks now precede every later link (reached on
      // its not-equal edge), so fold them into the prefix. The body's runtime
      // path is that prefix, then the ordinal-setting blocks and the switch.
      auto link_blocks = link.path_blocks();
      prefix.insert(prefix.end(), link_blocks.begin(), link_blocks.end());
      std::vector<cfg::Block*> path = prefix;
      path.insert(path.end(), ordinal_set_blocks.begin(),
                  ordinal_set_blocks.end());
      path.push_back(ord_switch_block);
      leaf_paths[dest_it->second].push_back(std::move(path));
      // Follow the not-equal path: another equals (collision) or, when the
      // bucket is exhausted, the ordinal switch (handled at loop top).
      cur = neq_e->target();
    }
  }

  // Default destination = the ordinal switch's default body.
  key_to_case[StringSwitchInfo::StringKey(StringSwitchInfo::DefaultCase{})] =
      default_body;

  // Bijection: #literals == #non-default ordinal cases (the ordinal switch is
  // validated to have a default, so non-default count == size - 1).
  if (seen_literals.size() != ord_sef.key_to_case().size() - 1) {
    return false;
  }

  auto region = region_from_leaf_paths(origin_block, leaf_paths);
  cfg::Block* hashcode_block = m_ctx.block_of(hashcode_insn);
  if (!finalize_region(origin_block, hashcode_block, region, leaf_paths)) {
    return false;
  }

  m_info.form = StringSwitchInfo::Form::HASH_SWITCH;
  m_info.origin_insn = hash_switch_insn;
  m_info.origin_block = origin_block;
  m_info.hashcode_insn = hashcode_insn;
  m_info.hashcode_block = hashcode_block;
  m_info.subject_reg = subject_reg;
  m_info.key_to_case = std::move(key_to_case);
  m_info.region_blocks = std::move(region);
  return true;
}

bool StringSwitchFinder::decode_equals_chain(
    reg_t subject_reg,
    const sparta::PatriciaTreeSet<live_range::Def>& subject_defs,
    cfg::Block* origin_block) {
  auto& cfg = m_ctx.cfg();
  const auto& ud = m_ctx.use_def();
  auto* hashcode_insn = m_hashcode_insn->insn;

  StringSwitchInfo::KeyToCase key_to_case;
  std::vector<const DexString*> chain_order;
  UnorderedSet<const DexString*> seen_literals;
  IRInstruction* origin_insn = nullptr;
  cfg::Block* default_block = nullptr;

  // Per-leaf runtime paths (origin included but exempt from hauling). The chain
  // is a single spine, so `prefix` accumulates the failed-test blocks; each
  // body is reached after its preceding tests fail, and the default after all
  // do.
  LeafPaths leaf_paths;
  std::vector<cfg::Block*> prefix;

  cfg::Block* cur = origin_block;
  // The chain advances via not-equal edges (not the happy path).
  CycleGuard guard;
  while (guard.visit(cur)) {
    // Locate the next equals test. Outside a try the hashCode null-guard and
    // the first equals share the origin block; inside a try the may-throw
    // hashCode invoke and each const-string/equals end their own blocks, so the
    // equals is one or more goto hops past `cur` -- all spanned by the
    // BigBlock.
    EqualsLink link = find_equals_link(cur, subject_defs);
    if (!link.found()) {
      // No further equals: `cur` (the last not-equal edge's target) is the
      // default body entry, reached after every test on the spine failed.
      default_block = cur;
      leaf_paths[default_block].push_back(prefix);
      break;
    }

    cfg::Edge* equal_e = nullptr;
    cfg::Edge* neq_e = nullptr;
    if (!equal_neq_edges(cfg, ud, link.branch_block(), link.equals_insn,
                         &equal_e, &neq_e)) {
      return false;
    }
    always_assert(equal_e != nullptr && neq_e != nullptr);
    if (origin_insn == nullptr) {
      origin_insn =
          link.branch_block()->get_last_insn()->insn; // first dispatch branch
    }
    if (!seen_literals.insert(link.literal).second) {
      return false; // duplicate literal
    }
    // The equal edge goes directly to the case body. Distinct literals may
    // share a body (e.g. `case "a": case "b":`), so destinations need not be
    // distinct.
    key_to_case[StringSwitchInfo::StringKey(link.literal)] = equal_e->target();
    chain_order.push_back(link.literal);

    // This matching link's blocks extend the prefix; the equal edge goes
    // straight to the body, so the body's runtime path is exactly the prefix so
    // far. This link also stays in the prefix (later links continue from its
    // not-equal edge). A shared body accumulates one path per literal reaching
    // it.
    auto link_blocks = link.path_blocks();
    prefix.insert(prefix.end(), link_blocks.begin(), link_blocks.end());
    leaf_paths[equal_e->target()].push_back(prefix);
    cur = neq_e->target();
  }

  if (default_block == nullptr || key_to_case.empty()) {
    return false;
  }
  key_to_case[StringSwitchInfo::StringKey(StringSwitchInfo::DefaultCase{})] =
      default_block;

  auto region = region_from_leaf_paths(origin_block, leaf_paths);
  cfg::Block* hashcode_block = m_ctx.block_of(hashcode_insn);
  if (!finalize_region(origin_block, hashcode_block, region, leaf_paths)) {
    return false;
  }

  m_info.form = StringSwitchInfo::Form::EQUALS_CHAIN;
  m_info.origin_insn = origin_insn;
  m_info.origin_block = origin_block;
  m_info.hashcode_insn = hashcode_insn;
  m_info.hashcode_block = hashcode_block;
  m_info.subject_reg = subject_reg;
  m_info.key_to_case = std::move(key_to_case);
  m_info.region_blocks = std::move(region);
  m_info.chain_order = std::move(chain_order);
  return true;
}

bool StringSwitchFinder::accumulate_leaf_loads(
    const LeafPaths& leaf_paths,
    const UnorderedSet<IRInstruction*>& escaping,
    StringSwitchInfo::ExtraLoads* out) {
  // The paths are the real (constant-propagation-resolved) dispatch paths, so
  // this just replays what recovery already learned -- no re-analysis of the
  // ordinal selector.
  for (const auto& [leaf, paths] : UnorderedIterable(leaf_paths)) {
    std::optional<SwitchEquivFinder::InstructionSet> merged;
    for (const auto& path : paths) {
      auto loads = loads_along_path(path, escaping);
      if (!merged) {
        merged = std::move(loads);
      } else if (*merged != loads) {
        return false; // divergent loads reach this leaf
      }
    }
    if (merged && !merged->empty()) {
      (*out)[leaf] = std::move(*merged);
    }
  }
  return true;
}

UnorderedSet<cfg::Block*> StringSwitchFinder::region_from_leaf_paths(
    cfg::Block* origin_block, const LeafPaths& leaf_paths) {
  // Each leaf path is the ordered run of machinery blocks a real execution
  // traverses to reach that leaf, so their union is exactly the region. The
  // origin is added explicitly: the hash-switch paths begin after it, so it is
  // not necessarily on any path (the equals-chain paths do start at it).
  UnorderedSet<cfg::Block*> region;
  region.insert(origin_block);
  for (const auto& [leaf, paths] : UnorderedIterable(leaf_paths)) {
    for (const auto& path : paths) {
      for (cfg::Block* b : path) {
        region.insert(b);
      }
    }
  }
  return region;
}

bool StringSwitchFinder::finalize_region(
    cfg::Block* origin_block,
    cfg::Block* hashcode_block,
    const UnorderedSet<cfg::Block*>& region,
    const LeafPaths& leaf_paths) {
  const auto& du = m_ctx.def_use();

  // Exceptional exits: the M Throwable handlers shared uniformly by R.
  if (!valid_catch_exits(region, hashcode_block)) {
    return false;
  }

  // Self-containment + the instruction allowlist + escape accounting, in a
  // single pass over R.
  //
  // Self-containment: every region block (except the entry) must be reachable
  // only from within R. Throw edges leaving R (to catch handlers) are
  // exceptional exits (validated by valid_catch_exits above), but no machinery
  // block may itself be a catch handler: that would be an exceptional entry
  // into R from outside its normal control flow, so R is not a self-contained
  // unit we can rewrite.
  //
  // Escape: a value defined in R and used outside R must be a haulable constant
  // load (`escaping` collects these). A const-string is NOT haulable -- it can
  // throw, so relocating it could require new catch handlers -- and any other
  // escaping value (e.g. a CSE'd hashCode result reused in a body) means R is
  // not self-contained: reject.
  UnorderedSet<IRInstruction*> escaping;
  for (auto* b : UnorderedIterable(region)) {
    if (b->is_catch()) {
      return false;
    }
    // The origin may contain arbitrary code preceding the hashCode + switch, so
    // it is exempt from the pred check, the instruction allowlist, and escape
    // accounting: planned transforms keep the origin block largely intact (only
    // its terminating branch and successor edges change), so any value it
    // defines still dominates its later uses and need not be hauled.
    if (b == origin_block) {
      continue;
    }
    for (auto* e : b->preds()) {
      if (region.count(e->src()) == 0u) {
        return false; // external predecessor into the switching machinery
      }
    }
    for (auto& mie : InstructionIterable(b)) {
      auto* def = mie.insn;
      if (!is_permissible_region_insn(def)) {
        return false; // extraneous, non-switching instruction in the region
      }
      auto it = du.find(def);
      if (it == du.end()) {
        continue;
      }
      bool haulable =
          opcode::is_a_literal_const(def->opcode()) && def->has_dest();
      bool escapes = false;
      for (const auto& use : UnorderedIterable(it->second)) {
        auto* ub = m_ctx.block_of(use.insn);
        if (ub != nullptr && region.count(ub) != 0u) {
          continue; // internal use
        }
        if (ub != nullptr && ub->is_catch()) {
          // A region value consumed on an exceptional path may have ambiguity
          // when hauling.
          return false;
        }
        if (!haulable) {
          return false; // non-haulable value escapes the region
        }
        escapes = true;
      }
      if (escapes) {
        escaping.insert(def);
      }
    }
  }

  // Record, per leaf, the escaping consts on the runtime path(s) recovery
  // captured to reach it. A leaf reached by paths with divergent loads has a
  // path-dependent value that cannot be hauled, so accumulation fails and we
  // recover no switch. The common case (no escaping consts) skips this entirely
  // and leaves `extra_loads` empty.
  StringSwitchInfo::ExtraLoads extra_loads;
  if (!escaping.empty() &&
      !accumulate_leaf_loads(leaf_paths, escaping, &extra_loads)) {
    return false;
  }
  m_info.extra_loads = std::move(extra_loads);
  return true;
}

bool may_contain_string_switch(cfg::ControlFlowGraph& cfg) {
  bool has_hashcode = false;
  bool has_equals = false;
  for (auto& mie : InstructionIterable(cfg)) {
    if (is_string_hashcode(mie.insn)) {
      has_hashcode = true;
    } else if (is_string_equals(mie.insn)) {
      has_equals = true;
    }
    if (has_hashcode && has_equals) {
      return true; // both present; a string switch is possible
    }
  }
  return false;
}

std::vector<StringSwitchInfo> find_string_switches(
    const StringSwitchCfgContext& ctx) {
  auto& cfg = ctx.cfg();

  std::vector<StringSwitchInfo> results;
  UnorderedSet<cfg::Block*> consumed;
  // Anchor on every String.hashCode() call, in deterministic block order.
  auto iterable = InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (!is_string_hashcode(it->insn)) {
      continue;
    }
    if (consumed.count(it.block()) != 0u) {
      continue; // this hashCode is already inside a recovered region
    }
    StringSwitchFinder finder(ctx, it);
    if (!finder.success()) {
      continue;
    }
    const auto& region = finder.info().region_blocks;
    bool overlap = false;
    for (auto* rb : UnorderedIterable(region)) {
      if (consumed.count(rb) != 0u) {
        overlap = true;
        break;
      }
    }
    if (overlap) {
      continue;
    }
    for (auto* rb : UnorderedIterable(region)) {
      consumed.insert(rb);
    }
    results.push_back(finder.info());
  }
  return results;
}

std::vector<StringSwitchInfo> find_string_switches(
    cfg::ControlFlowGraph& cfg,
    const std::shared_ptr<cp::intraprocedural::FixpointIterator>& fixpoint) {
  StringSwitchCfgContext ctx(cfg, fixpoint);
  return find_string_switches(ctx);
}

void copy_extra_loads_to_leaf_blocks(
    cfg::ControlFlowGraph& cfg,
    const StringSwitchInfo::ExtraLoads& extra_loads) {
  for (const auto& [leaf, loads] : UnorderedIterable(extra_loads)) {
    SwitchEquivEditor::copy_extra_loads_to_leaf_block(extra_loads, &cfg, leaf);
  }
}
