/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ControlFlow.h"
#include "IRInstruction.h"
#include "Match.h"
#include "MatchFlowDetail.h"

namespace mf {

struct flag_t;
struct location_t;
struct result_t;

/**
 * Data Flow Matching
 * =============================================================================
 *
 * A mechanism for describing predicates over a program's data-flow graph, in
 * two parts:
 *
 *  - Constraints over individual instructions, using m::match_t predicates.
 *  - Constraints over data-dependencies between instructions (e.g. that the
 *    operand to one instruction should be the result of an instruction matching
 *    some further constraint itself, and so on, transitively).
 *
 *
 * Defining Matchers
 * -----------------------------------------------------------------------------
 *
 * Predicates are represented as a graph with instruction constraints as nodes
 * and flow constraints as edges.
 *
 *   template <typename M>
 *   location_t flow_t::insn(m::match_t<IRInstruction*, M> m)
 *
 * is used to introduce a new instruction constraint, predicated on the
 * instruction matcher `m`.  It returns a reference to that constraint which can
 * be used to introduce flow-constraints from or to it, or query results for
 * instructions matching it.
 *
 *   location_t location_t::src(src_index_t ix, location_t l, flag_t flags)
 *
 * is used to introduce a new flow constraint, requiring that an instruction
 * matching the constraint at `this` location must have its `ix`-th operand
 * supplied by an instruction matching the constraint at `l`, subject to the
 * modifiers imposed by the `flags` (see "Flags", below).
 *
 *   location_t
 *   location_t::srcs_from(src_index_t lb, location_t l, flag_t flags)
 *
 * is a variant of `srcs` that introduces a flow constraint for all operands
 * with indices greater than or equal to `lb`.
 *
 * A reference to the target location is returned, to allow calls to `src` and
 * `srcs_from` to be chained:
 *
 *   auto a = f.insn(...)
 *       .src(2, z)
 *       .src(0, x)
 *       .src(2, y)
 *       .srcs_from(4, z)
 *       .srcs_from(1, w)
 *
 * Flow constraints have a precedence order.  Constraints on individual operands
 * have highest precedence, followed by range constraints introduced by
 * `srcs_from`, in order of distance from the operand in question.  In the
 * chaining example above, instructions satisfying `a` will have operands
 * satisfying the following constraints:
 *
 *   0 1 2 3 4 5 ...
 *   x w y w z z ...
 *
 * The order in which constraints are introduced only affects precedence for
 * two constraints impacting precisely the same set of operands, in which case
 * the later constraint takes precedence.
 *
 * It is not possible to share locations that originate from different `flow_t`
 * instances.  This will throw an exception at runtime.
 *
 *
 * Flags
 * -----------------------------------------------------------------------------
 *
 * Flags modify flow constraints.  When discussing the effect of flags below,
 * consider the following bytecode (assume there are entrypoints into A, B, D,
 * and F):
 *
 *       ...
 *   A:  const r 0
 *       goto  :R
 *
 *   B:  const a 1
 *       move  b a
 *   C:  move  r b
 *       goto  :R
 *
 *   D:  invoke-static LFoo;.bar:()I
 *   E:  move-result r
 *       goto  :R
 *
 *   F:  invoke-static LFoo;.baz:()I
 *       move-result a
 *   G:  move  r a
 *       goto  :R
 *
 *   R:  return r
 *
 *
 * Alias Flags determine how far to search for candidate instructions:
 *
 * mf::dest - (default) look at the instructions whose destination register
 *     directly fills the source register.  In the example above, instructions
 *     labelled A, C, E, and G are `dest` candidates.
 *
 * mf::alias - look for candidate instructions by following zero or more
 *     move or move-result instructions.  The moves/move-results themselves are
 *     ignored.  In the above example, A, B, D, and F are `alias` candidates.
 *
 * mf::result - look for candidate instructions optionally behind a move-result.
 *     move-results themselves are ignored.  In the above example, A, C, D, G
 *     are `result` candidates.
 *
 *
 * Quant Flags determine how many candidates (instructions found by following
 * the rules for the provided alias flag) should match the constraint for the
 * operand to be considered consistent:
 *
 * mf::exists - (default) at least one candidate instruction must match the
 *     constraint.
 *
 * mf::forall - all candidate instructions must match the constraint.
 *
 * mf::unique - there must be exactly one candidate instruction, and it must
 *     match the constraint.
 *
 *
 * Querying Results
 * -----------------------------------------------------------------------------
 *
 * A predicate is applied over a CFG using `find`:
 *
 *   result_t flow_t::find(ControlFlowGraph& cfg, location_t l)
 *   result_t flow_t::find(ControlFlowGraph& cfg,
 *                         std::initializer_list<location_t> ls)
 *
 * The result is a sub-graph of the data-flow graph reachable by following edges
 * matching flow constraints, backwards starting from instructions matching root
 * locations.  There can be a single root location (`l`), or multiple (`ls`).
 * This data structure can be queried in two ways:
 *
 *   result_t::matching(location_t l)
 *   result_t::matching(location_t l, IRInstruction* insn, src_index_t ix)
 *
 * Both queries return an iterable range of instructions.
 *
 * The first query returns all instructions matching the constraint at `l`,
 * reachable from the root passed to `find`.
 *
 * The second query returns all instructions that could supply the `ix`-th
 * operand to `insn`, when `insn` matches the constraint at `l`.  It could
 * return a different set of instructions for the same instruction and operand,
 * given a different location, e.g.
 *
 *   flow_t f;
 *
 *   auto odd  = f.insn(m::const_(m::has_literal(is_odd)));
 *   auto even = f.insn(m::const_(m::has_literal(is_even)));
 *   auto addo = f.insn(m::add_int_()).src(0, odd);
 *   auto adde = f.insn(m::add_int_()).src(0, even);
 *   auto sub  = f.insn(m::sub_int_()).src(0, addo).src(1, adde);
 *
 * when applied to some code (assuming some entrypoints into X and Y):
 *
 *     ...
 *  X: const   a 0
 *     goto    :Z
 *
 *  Y: const   a 1
 *     goto    :Z
 *
 *  Z: const   b 0
 *  W: add-int c a b
 *  U: sub-int d c c
 *
 * and then queried:
 *
 *   auto res = f.find(cfg, sub);
 *
 *   res.matching(addo, W, 0); // = {X}
 *   res.matching(adde, W, 0); // = {Y}
 *
 * ...will yield different results for W's first operand, depending on the
 * location.
 *
 * NB. `res.matching(addo, W, 1)` is empty, because `addo`'s second operand is
 * not constrained.  It can be made to produce `Z` by adding another constraint:
 *
 *   auto any = f.insn(m::any<IRInstruction*>());
 *   addo.src(1, any);
 *
 * NB. In the following predicate, the `lit` location occurs in two flow
 * constraints:
 *
 *   flow_t f;
 *   auto lit = f.insn(m::const_());
 *   auto add = f.insn(m::add_int_()).src(0, lit).src(1, lit);
 *
 *   auto res = f.find(cfg, add);
 *
 * `res` finds add instructions where both operands are constants, NOT add
 * instructions where both operands are the SAME const instruction. I.e. the
 * following suffices:
 *
 *     const   a 0
 *     const   b 1
 *     add-int c a b
 */
struct flow_t {
  /**
   * Add a new instruction constraint to this predicate.
   *
   * Returns a location_t that can be used to refer to instructions matching
   * this constraint.
   */
  template <typename M>
  location_t insn(m::match_t<IRInstruction*, M> m);

  /**
   * Search for sub-trees originating from instructions matching the constraints
   * at ls or l, in the given control-flow graph.  This operation requires that
   * a unique exit block exists in `cfg`, and will calculate one (mutating the
   * CFG) if it does not exist.
   */
  result_t find(cfg::ControlFlowGraph& cfg, location_t l) const;
  result_t find(cfg::ControlFlowGraph& cfg,
                std::initializer_list<location_t> ls) const;

 private:
  friend struct location_t;

  std::vector<detail::Constraint> m_constraints;
};

constexpr detail::AliasFlag dest = detail::AliasFlag::dest;
constexpr detail::AliasFlag alias = detail::AliasFlag::alias;
constexpr detail::AliasFlag result = detail::AliasFlag::result;
constexpr detail::QuantFlag exists = detail::QuantFlag::exists;
constexpr detail::QuantFlag forall = detail::QuantFlag::forall;
constexpr detail::QuantFlag unique = detail::QuantFlag::unique;

struct flag_t {
  constexpr flag_t() = default;

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  /* implicit */ constexpr flag_t(detail::AliasFlag a) : m_alias{a} {}

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  /* implicit */ constexpr flag_t(detail::QuantFlag q) : m_quant{q} {}

  constexpr flag_t(detail::AliasFlag a, detail::QuantFlag q)
      : m_alias{a}, m_quant{q} {}

 private:
  friend struct location_t;

  detail::AliasFlag m_alias{};
  detail::QuantFlag m_quant{};
};

namespace detail {
// Re-open the detail namespace to define operator|, because ADL only adds the
// innermost namespaces for parameter types to the lookup set.

constexpr flag_t operator|(AliasFlag a, QuantFlag q) { return {a, q}; }
constexpr flag_t operator|(QuantFlag q, AliasFlag a) { return {a, q}; }

} // namespace detail

struct location_t {
  /**
   * A null location. Cannot be used in a `find()` invocation.
   */
  location_t() : location_t(nullptr, 0) {}

  /**
   * Add a data-flow constraint:  The operand referred to by ix must be supplied
   * by an instruction matching the constraint at l.
   *
   * flags modify the nature of the constraint and come in two varieties: An
   * AliasFlag and a QuantFlag.  At most one of each can be supplied.  If a
   * variety of flag is not supplied, a default is chosen.  Consult the
   * documentation for each flag to see how they modify constraints and which is
   * the default.
   */
  location_t src(src_index_t ix, location_t l, flag_t flags = {});

  /**
   * Add a data-flow constraint to operands with indices at or above lb.
   *
   * See location_t::src for an explanation of the l and flags parameters.
   */
  location_t srcs_from(src_index_t lb, location_t l, flag_t flags = {});

  bool operator==(const location_t& other) const {
    return std::tie(m_owner, m_ix) == std::tie(other.m_owner, other.m_ix);
  }

  bool operator!=(const location_t& other) const { return !(*this == other); }

 private:
  friend struct flow_t;
  friend struct result_t;
  location_t(flow_t* owner, detail::LocationIx ix) : m_owner(owner), m_ix(ix) {}

  /** Access the underlying constraint */
  detail::Constraint& constraint();

  flow_t* m_owner;
  detail::LocationIx m_ix;
};

struct result_t {
  template <typename It>
  struct range {
    range(It begin, It end) : m_begin(begin), m_end(end) {}

    static range empty() {
      It it;
      return {it, it};
    }

    explicit operator bool() const { return m_begin != m_end; }

    /**
     * If the range contains precisely one instruction, returns it, otherwise
     * returns nullptr.
     */
    IRInstruction* unique() const {
      return m_begin != m_end && std::next(m_begin) == m_end ? *m_begin
                                                             : nullptr;
    }

    It begin() const { return m_begin; }
    It end() const { return m_end; }

   private:
    It m_begin;
    It m_end;
  };

  struct insn_iterator {
    using underlying_iterator = detail::Instructions::const_iterator;

    using iterator_category = std::forward_iterator_tag;
    using value_type = IRInstruction*;
    using difference_type = underlying_iterator::difference_type;
    using pointer = IRInstruction* const*;
    using reference = IRInstruction* const&;

    insn_iterator() = default;

    insn_iterator& operator++();
    insn_iterator operator++(int);

    IRInstruction* const& operator*() const;

    bool operator==(const insn_iterator& that) const;
    bool operator!=(const insn_iterator& that) const;

   private:
    friend struct result_t;
    explicit insn_iterator(underlying_iterator it) : m_it(it) {}

    underlying_iterator m_it;
  };

  using src_iterator = detail::Source::const_iterator;

  using insn_range = range<insn_iterator>;
  using src_range = range<src_iterator>;

  /**
   * Return all instructions referred to by l in these results. Beware, the
   * resulting range is unordered.
   */
  insn_range matching(location_t l) const;

  /**
   * Orders the elements of an unordered instruction range.
   */
  std::vector<IRInstruction*> order(insn_range range) const;

  /**
   * Assuming insn is referred to by l in these results, returns all the
   * instructions that could supply its ix-th operand, and satisfy the ix-th
   * data-flow constraint on l.  If insn is not matched by the constraint at l,
   * an empty range is returned.
   */
  src_range matching(location_t l,
                     const IRInstruction* insn,
                     src_index_t ix) const;

 private:
  friend struct flow_t;
  friend std::ostream& operator<<(std::ostream&, const result_t&);

  /** result_t instances are only constructible by flow_t::find. */
  explicit result_t(detail::Locations results,
                    std::shared_ptr<detail::Order> order)
      : m_results(std::move(results)), m_order(std::move(order)) {}

  detail::Locations m_results;
  std::shared_ptr<detail::Order> m_order;
};

template <typename M>
inline location_t flow_t::insn(m::match_t<IRInstruction*, M> m) {

  detail::LocationIx ix = m_constraints.size();
  m_constraints.emplace_back(mf::detail::insn_matcher(m));

  return location_t{this, ix};
}

inline location_t location_t::src(src_index_t ix, location_t l, flag_t flags) {
  always_assert(m_owner != nullptr && m_owner == l.m_owner &&
                "location_t shared between flow_t instances.");
  constraint().add_src(ix, l.m_ix, flags.m_alias, flags.m_quant);
  return *this;
}

inline location_t location_t::srcs_from(src_index_t lb,
                                        location_t l,
                                        flag_t flags) {
  always_assert(m_owner != nullptr && m_owner == l.m_owner &&
                "location_t shared between flow_t instances.");
  constraint().add_src_range(lb, l.m_ix, flags.m_alias, flags.m_quant);
  return *this;
}

inline detail::Constraint& location_t::constraint() {
  return m_owner->m_constraints[m_ix];
}

inline result_t::insn_iterator& result_t::insn_iterator::operator++() {
  ++m_it;
  return *this;
}

inline result_t::insn_iterator result_t::insn_iterator::operator++(int) {
  return insn_iterator{m_it++};
}

inline IRInstruction* const& result_t::insn_iterator::operator*() const {
  return m_it->first;
}

inline bool result_t::insn_iterator::operator==(
    const result_t::insn_iterator& that) const {
  return m_it == that.m_it;
}

inline bool result_t::insn_iterator::operator!=(
    const result_t::insn_iterator& that) const {
  return !(*this == that);
}

} // namespace mf
