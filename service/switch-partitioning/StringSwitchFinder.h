/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "BigBlocks.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "SwitchEquivFinder.h"

/**
 * Recovered description of ONE string switch (see StringSwitchFinder below). A
 * plain value type so the driver `find_string_switches` can return many.
 *
 * The recovered control flow forms a region R with exactly one ENTRY (the
 * `origin` branch), one exit per matched string literal, one default exit, and
 * M
 * (>= 0) exits, one per Throwable type caught by an enclosing try/catch.
 */
struct StringSwitchInfo {
  enum class Form {
    // `subject.hashCode()`; switch(hash); per bucket chained equals() set an
    // ordinal; switch(ordinal) -> bodies.
    HASH_SWITCH,
    // No hash switch; a linear chain of `if (equals(subject, lit))` branching
    // directly to bodies (hashCode() present only as a discarded null-guard).
    EQUALS_CHAIN,
  };

  // Reuse SwitchEquivFinder's default-case sentinel for consistency.
  using DefaultCase = SwitchEquivFinder::DefaultCase;

  // A case key is either a concrete (interned) String value, or the default.
  using StringKey = std::variant<DefaultCase, const DexString*>;

  struct key_comparator {
    bool operator()(const StringKey& l, const StringKey& r) const;
  };

  // string-value | default -> the block that ultimately executes for that case
  // (the second-stage / body block). This is the primary output.
  using KeyToCase = std::map<StringKey, cfg::Block*, key_comparator>;

  // One per caught Throwable type (the "M exits"), in handler order. Mirrors
  // cfg::ThrowInfo: `catch_type == nullptr` denotes a catch-all handler, and
  // `index` is the runtime catch-checking order (0 = tried first). The index is
  // retained so the throw edges can be re-attached faithfully when the region
  // is rewritten to an alternate form.
  struct CatchExit {
    const DexType* catch_type;
    uint32_t index;
    cfg::Block* handler;
  };

  // Constant loads defined inside R whose value is consumed at/after an exit
  // and therefore must be re-materialized ("hauled") into the destination on
  // rewrite. Same shape/meaning as SwitchEquivFinder::ExtraLoads
  // (block -> {reg -> const-load insn}).
  using ExtraLoads = SwitchEquivFinder::ExtraLoads;

  Form form{Form::HASH_SWITCH};
  // The first branching instruction of the region.
  IRInstruction* origin_insn{nullptr};
  cfg::Block* origin_block{nullptr};
  // The String.hashCode() call that anchors the switch.
  IRInstruction* hashcode_insn{nullptr};
  // The block containing `hashcode_insn`. For HASH_SWITCH inside a try region
  // this is a predecessor of `origin_block` (the may-throw hashCode invoke ends
  // its own block); otherwise it coincides with the dispatch entry. It is NOT a
  // region block -- it may hold arbitrary preceding code and arbitrary
  // predecessors -- but is retained so its exception edges can be validated
  // against (and reattached uniformly with) the region's.
  cfg::Block* hashcode_block{nullptr};
  // The String register being switched on.
  reg_t subject_reg{0};
  // case (string | default) -> destination block.
  KeyToCase key_to_case;
  // Internal ("non-leaf") blocks: the switching machinery, dead after rewrite.
  UnorderedSet<cfg::Block*> region_blocks;
  // EQUALS_CHAIN only: literals in source/execution order (for hotness
  // reorder).
  std::vector<const DexString*> chain_order;
  // The M catch exits (empty when not inside a try).
  std::vector<CatchExit> catch_exits;
  // Constants to haul into destinations on rewrite.
  ExtraLoads extra_loads;

  // The default destination, if present.
  [[nodiscard]] std::optional<cfg::Block*> default_case() const;
};

/**
 * Per-CFG information that StringSwitchFinder needs but that is invariant
 * across the (possibly many) string switches in one CFG. Compute it ONCE and
 * share it, rather than recomputing reaching-def chains and the
 * instruction->block map for every candidate.
 *
 * The fixpoint must be built with StringSwitchFinder::Analyzer (so String/class
 * constants are resolvable) and must already have been run().
 */
class StringSwitchCfgContext {
 public:
  StringSwitchCfgContext(
      cfg::ControlFlowGraph& cfg,
      std::shared_ptr<constant_propagation::intraprocedural::FixpointIterator>
          fixpoint);

  cfg::ControlFlowGraph& cfg() const { return m_cfg; }
  const std::shared_ptr<
      constant_propagation::intraprocedural::FixpointIterator>&
  fixpoint() const {
    return m_fixpoint;
  }
  const live_range::UseDefChains& use_def() const { return m_use_def; }
  const live_range::DefUseChains& def_use() const { return m_def_use; }
  cfg::Block* block_of(IRInstruction* insn) const;

 private:
  cfg::ControlFlowGraph& m_cfg;
  std::shared_ptr<constant_propagation::intraprocedural::FixpointIterator>
      m_fixpoint;
  live_range::UseDefChains m_use_def;
  live_range::DefUseChains m_def_use;
  UnorderedMap<IRInstruction*, cfg::Block*> m_insn_to_block;
};

/**
 * StringSwitchFinder recognizes the multi-stage control flow that javac/d8 emit
 * for `switch (String)` (and hand-written / tooling-generated equivalents),
 * rooted at a single `String.hashCode()` call, and recovers a canonical,
 * transform-grade StringSwitchInfo.
 *
 * STRICTNESS. `success()` is true only when the region is provably a
 * self-contained string dispatch that is safe to rewrite to an alternate form.
 * False negatives are acceptable; false positives are not. It verifies:
 *   - SELF-CONTAINMENT: every internal (non-leaf) block of R is reachable only
 *     from within R (no external predecessors), so R can be excised.
 *   - PERMISSIBLE INSTRUCTIONS: every instruction in R is part of the switching
 *     machinery (const/move loads, the String.hashCode/equals invokes, switches
 *     and conditional branches) -- no interleaved, unrelated logic.
 *   - NO ESCAPING NON-CONST VALUES: a value defined in R and used at/after an
 *     exit must be a literal constant (recorded in `extra_loads` to be hauled);
 *     any other escape => not a supported switch.
 *   - BIJECTION/COUNTS: recovered string keys are in 1:1 correspondence with
 * the reachable destinations; every literal's java_hashcode matches its bucket.
 *
 * READ-ONLY. Never mutates the CFG.
 *
 * RELATIONSHIP TO SwitchEquivFinder. The integer sub-dispatches (the hash
 * switch and the ordinal switch) are recovered by SwitchEquivFinder. The
 * String-specific decode (hashCode + equals -> string key) is performed here.
 */
class StringSwitchFinder {
 public:
  // The CP analyzer the context's fixpoint MUST be built with.
  using Analyzer = SwitchEquivFinder::Analyzer;

  // Analyzes the string switch anchored at `hashcode_insn` (an iterator at an
  // `invoke-virtual String.hashCode()`). Runs eagerly; query `success()`.
  StringSwitchFinder(const StringSwitchCfgContext& ctx,
                     const cfg::InstructionIterator& hashcode_insn);

  StringSwitchFinder() = delete;
  StringSwitchFinder(const StringSwitchFinder&) = delete;
  StringSwitchFinder& operator=(const StringSwitchFinder&) = delete;

  // True iff a self-contained, rewrite-safe string switch was recovered.
  [[nodiscard]] bool success() const { return m_success; }

  // The recovered switch. Only valid when `success()`.
  [[nodiscard]] const StringSwitchInfo& info() const { return m_info; }

 private:
  // Identifies the subject and dispatches to the appropriate form decoder.
  bool analyze();

  // Form A: `subject.hashCode()` feeds the switch `hash_switch_insn` in
  // `origin_block`.
  bool decode_hash_switch(
      reg_t subject_reg,
      const sparta::PatriciaTreeSet<live_range::Def>& subject_defs,
      cfg::Block* origin_block,
      IRInstruction* hash_switch_insn);

  // Form B: a linear chain of `if (equals(subject, lit))` anchored by a
  // (discarded) hashCode null-guard in `origin_block`.
  bool decode_equals_chain(
      reg_t subject_reg,
      const sparta::PatriciaTreeSet<live_range::Def>& subject_defs,
      cfg::Block* origin_block);

  // Shared strict validation over the recovered region: self-containment, the
  // instruction allowlist, const-only escape/haul accounting, and
  // exception-exit collection. Populates `m_info.extra_loads` and
  // `m_info.catch_exits`. The origin block is exempt from the allowlist/pred
  // checks (it stays intact).
  bool finalize_region(cfg::Block* origin_block,
                       cfg::Block* hashcode_block,
                       const UnorderedSet<cfg::Block*>& region);

  // One "link" of the dispatch: the BigBlock beginning at a head block, plus
  // the located `subject.equals(lit)` test within it. Inside a try the
  // const-string literal, the equals invoke, and the consuming branch are split
  // across blocks by throw edges; the BigBlock spans them (so its constituent
  // blocks are the region machinery and its last block holds the consuming
  // conditional branch). `big_block` is empty when `start` was interior to
  // another big block; `equals_insn` is null when the big block holds no
  // subject-equals (the chain has ended).
  struct EqualsLink {
    std::optional<big_blocks::BigBlock> big_block;
    IRInstruction* equals_insn{nullptr};
    // The constant String compared at the equals() call (read from the
    // StringDomain just before it); null if there is no such constant.
    const DexString* literal{nullptr};

    // True iff this link is a fully-recovered `subject.equals("constant")`
    // test: a big block was formed, it contains the subject-equals call, and
    // that call's argument is a constant String. When this holds,
    // `equals_insn`, `literal`, and the accessors below are all valid -- so
    // callers need no further checks.
    bool found() const {
      return big_block.has_value() && equals_insn != nullptr &&
             literal != nullptr;
    }

    // The block whose conditional branch consumes the equals result -- the big
    // block's last block.
    cfg::Block* branch_block() const { return big_block->get_last_block(); }

    // Adds the big block's constituent blocks to `region`, stopping before
    // `until` (if given). A no-op when no big block was formed.
    void add_to_region(UnorderedSet<cfg::Block*>* region,
                       cfg::Block* until = nullptr) const {
      if (!big_block) {
        return;
      }
      for (cfg::Block* b : big_block->get_blocks()) {
        if (b == until) {
          break;
        }
        region->insert(b);
      }
    }
  };

  // Builds the BigBlock starting at `start` (a "happy path" run of blocks split
  // only by throw edges) and locates the first `subject.equals(lit)` test in
  // it.
  EqualsLink find_equals_link(
      cfg::Block* start,
      const sparta::PatriciaTreeSet<live_range::Def>& subject_defs) const;

  // Validates the region's exception edges and records its Throwable exits.
  // Every block that can throw (in `region`, plus `hashcode_block`) must carry
  // an identical, ordered set of throw edges; divergent or intermittent edges
  // are rejected. On success populates `m_info.catch_exits` (empty when the
  // region is not inside a try).
  bool valid_catch_exits(const UnorderedSet<cfg::Block*>& region,
                         cfg::Block* hashcode_block);

  // Follows `equal_edge` (through goto-only blocks, which are added to
  // `region`) to the ordinal switch, returning the constant ordinal value
  // arriving there. The ordinal may be set in a block on the path, or preloaded
  // in a dominating block (in which case the equal edge targets the ordinal
  // switch directly); in both cases it is read from the exit state of the last
  // block before the ordinal switch.
  bool ordinal_from_equal_path(cfg::Edge* equal_edge,
                               cfg::Block* ord_switch_block,
                               reg_t ordinal_reg,
                               UnorderedSet<cfg::Block*>* region,
                               int32_t* out) const;

  const StringSwitchCfgContext& m_ctx;
  const cfg::InstructionIterator& m_hashcode_insn;
  StringSwitchInfo m_info;
  bool m_success{false};
};

/**
 * Cheap pre-filter: a single linear scan returning true iff `cfg` contains at
 * least one String.hashCode() call AND at least one String.equals() call. Both
 * forms of a string switch require both calls, so when this returns false there
 * is no chance of recovering one -- callers can skip the (far more expensive)
 * constant-propagation fixpoint and find_string_switches() entirely. Short-
 * circuits as soon as both are seen.
 */
bool may_contain_string_switch(cfg::ControlFlowGraph& cfg);

/**
 * Driver: find every supported string switch in `cfg`, anchored on
 * String.hashCode() calls. `fixpoint` must be built with
 * StringSwitchFinder::Analyzer and already run(). Overlapping/duplicate regions
 * are de-duplicated. The CFG is not modified.
 */
std::vector<StringSwitchInfo> find_string_switches(
    cfg::ControlFlowGraph& cfg,
    const std::shared_ptr<
        constant_propagation::intraprocedural::FixpointIterator>& fixpoint);
