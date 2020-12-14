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
   * at l, in the given control-flow graph.
   */
  result_t find(const cfg::ControlFlowGraph& cfg, location_t l) const;

 private:
  friend struct location_t;

  std::vector<detail::Constraint> m_constraints;
};

struct location_t {
  /**
   * Add a data-flow constraint:  The operand referred to by ix must be supplied
   * by an instruction matching the constraint at l.
   */
  location_t src(src_index_t ix, location_t l);

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
 private:
  /**
   * Locations represents the following nested mapping:
   *
   *   location_t ->> IRInstruction* -> src_index_t ->> IRInstruction*
   *
   * Where ->> represents a multimap.  As all results come from a single
   * flow_t instance, the location_t can be referred to by its index which is
   * just a number.  These numbers are densely packed, so the multimap is
   * represented by a vector-of-pointers-to-maps with location indices serving
   * as keys.  The pointer indirection aims to save space in the case of an
   * empty mapping.
   *
   * Similarly, source indices are densely packed for an instruction, so the
   * inner multimap is represented by a vector-of-vectors, keyed by the source
   * index.
   */
  using Source = std::vector<IRInstruction*>;
  using Sources = std::vector<Source>;
  using Instructions = std::unordered_map<IRInstruction*, Sources>;
  using Locations = std::vector<std::unique_ptr<Instructions>>;

 public:
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
    using iterator_category = std::forward_iterator_tag;
    using value_type = IRInstruction*;
    using difference_type = Instructions::const_iterator::difference_type;
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
    explicit insn_iterator(Instructions::const_iterator it) : m_it(it) {}

    Instructions::const_iterator m_it;
  };

  using src_iterator = Source::const_iterator;

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
  explicit result_t(Locations results) : m_results(std::move(results)) {}

  Locations m_results;
};

template <typename M>
inline location_t flow_t::insn(m::match_t<IRInstruction*, M> m) {

  detail::LocationIx ix = m_constraints.size();
  m_constraints.emplace_back(mf::detail::insn_matcher(m));

  return location_t{this, ix};
}

inline location_t location_t::src(src_index_t src, location_t src_constraint) {
  always_assert(m_owner != nullptr && m_owner == src_constraint.m_owner &&
                "location_t shared between flow_t instances.");

  auto& src_locs = constraint().srcs;
  if (src_locs.size() <= src) {
    src_locs.resize(src + 1, detail::NO_LOC);
  }

  src_locs[src] = src_constraint.m_ix;
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
