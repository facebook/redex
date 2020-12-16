/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
   * at l, in the given control-flow graph.  This operation requires that a
   * unique exit block exists in `cfg`, and will calculate one (mutating the
   * CFG) if it does not exist.
   */
  result_t find(cfg::ControlFlowGraph& cfg, location_t l) const;

 private:
  friend struct location_t;

  std::vector<detail::Constraint> m_constraints;
};

/** Flags */

// (Default) Look for sources one step away from the current instruction, i.e.
//
//   A:  const   r, 0
//   B:  return  r
//
// B's source register would be matched by A.
constexpr detail::AliasFlag dest = detail::AliasFlag::dest;

// Look for sources by following zero or moves to the current instruction, i.e.
//
//   A:  const  r, 0
//       move   q, r
//       move   p, q
//           ...
//       move   a, b
//   B:  return a
//
// B's source register would be matched by A.
constexpr detail::AliasFlag alias = detail::AliasFlag::alias;

// Look for sources via a move-result to the instruction that fills the result
// register, i.e.
//
//   A:  invoke-static "LFoo;.bar:()I"
//       move-result r
//   B:  return      r
//
// B's source register would be matched by A, via the move-result.
constexpr detail::AliasFlag result = detail::AliasFlag::result;

// (Default) Source constraint is matched if at least one matching source
// exists.
constexpr detail::QuantFlag exists = detail::QuantFlag::exists;

// Source constraint is matched if all found sources that flow in (as per the
// AliasFlag) match the constraint.
constexpr detail::QuantFlag forall = detail::QuantFlag::forall;

// Source constraint is matched if exactly one source flows in (as per the
// AliasFlag) and it matches the constraint.
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

inline constexpr flag_t operator|(AliasFlag a, QuantFlag q) { return {a, q}; }
inline constexpr flag_t operator|(QuantFlag q, AliasFlag a) { return {a, q}; }

} // namespace detail

struct location_t {
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

    /**
     * If the range contains precisely one instruction, returns it, otherwise
     * returns nullptr.
     */
    IRInstruction* unique() {
      return std::next(m_begin) == m_end ? *m_begin : nullptr;
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
   * Return all instructions referred to by l in these results.
   */
  insn_range matching(location_t l) const;

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

  /** result_t instances are only constructible by flow_t::find. */
  explicit result_t(detail::Locations results)
      : m_results(std::move(results)) {}

  detail::Locations m_results;
};

template <typename M>
inline location_t flow_t::insn(m::match_t<IRInstruction*, M> m) {

  detail::LocationIx ix = m_constraints.size();
  m_constraints.emplace_back(mf::detail::insn_matcher(m));

  return location_t{this, ix};
}

inline location_t location_t::src(src_index_t src,
                                  location_t src_constraint,
                                  flag_t flags) {
  always_assert(m_owner != nullptr && m_owner == src_constraint.m_owner &&
                "location_t shared between flow_t instances.");

  auto& src_locs = constraint().srcs;
  if (src_locs.size() <= src) {
    src_locs.resize(src + 1, {detail::NO_LOC, {}, {}});
  }

  src_locs[src] = {src_constraint.m_ix, flags.m_alias, flags.m_quant};
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
