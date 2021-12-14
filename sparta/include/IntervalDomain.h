/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cassert>
#include <limits>
#include <ostream>

#include "AbstractDomain.h"

namespace sparta {

/*
 * Closed integer intervals with boundaries of type Num -- a bounded integral
 * type.
 *
 * The minimal and maximal elements of `Num` are designated as MIN and MAX
 * respectively.  Finite intervals smaller than or equal to (MIN, MAX) can be
 * represented precisely by this type.  Any overhang below MIN or above MAX
 * (inclusive) is approximated by "extending out to infinity":
 *
 *   [min, min] is approximated by [-inf, min]
 *   [max, max] is approximated by [max, +inf]
 *
 * Because of the handling of extremal values, it is recommended that Num be a
 * signed type, even when only non-negative values are interesting, as on an
 * unsigned type, 0 will take the position of MIN, causing a loss of precision:
 *
 *   [0, 0] + [1, 1] = [-inf, min] + [1, 1]
 *                   = [-inf, 1]
 *
 * _|_ has a special encoding of [MAX, MIN], making it the only inhabitant of
 * the type for which the upperbound is strictly smaller than the lowerbound.
 * This property is exploited for the implementation of is_bottom() and means
 * that code that assumes a sensible ordering of bounds must be guarded by a
 * check for `!is_bottom()`.
 */
template <typename Num>
class IntervalDomain final : public AbstractDomain<IntervalDomain<Num>> {
  static_assert(std::is_integral<Num>::value, "expecting integer bounds.");

 public:
  static constexpr Num MIN = std::numeric_limits<Num>::min();
  static constexpr Num MAX = std::numeric_limits<Num>::max();
  static_assert(MIN < MAX, "Encoding of Bottom requires this property.");

  static IntervalDomain bottom() { return {MAX, MIN}; }

  /* [lb, ub] */
  static IntervalDomain finite(Num lb, Num ub) {
    assert(MIN < lb && "interval not bounded below.");
    assert(lb <= ub && "interval inverted.");
    assert(ub < MAX && "interval not bounded above.");
    return {lb, ub};
  }

  /* [lb, +inf] */
  static IntervalDomain bounded_below(Num lb) {
    assert(MIN < lb && "interval underflow");
    return {lb, MAX};
  }

  /* [-inf, ub] */
  static IntervalDomain bounded_above(Num ub) {
    assert(ub < MAX && "interval overflow.");
    return {MIN, ub};
  }

  /* [max, +inf] */
  static IntervalDomain high() { return {MAX, MAX}; }

  /* [-inf, min] */
  static IntervalDomain low() { return {MIN, MIN}; }

  /* [-inf, +inf] */
  static IntervalDomain top() { return {MIN, MAX}; }

  /* Default constructor produces Top. */
  IntervalDomain() : IntervalDomain(MIN, MAX) {}

  /* Inclusive lower-bound of the interval, assuming interval is not bottom. */
  Num lower_bound() const {
    assert(!is_bottom());
    return m_lb;
  }

  /*
   * Inclusive upper-bound of the interval, assuming interval is not bottom.
   * Guaranteed to be greater than or equal to lower_bound().
   */
  Num upper_bound() const {
    assert(!is_bottom());
    return m_ub;
  }

  IntervalDomain& operator+=(const IntervalDomain& that) {
    if (that.is_bottom()) {
      set_to_bottom();
    } else if (!is_bottom()) {
      m_lb = m_lb == MIN ? m_lb : clamped_add(m_lb, that.m_lb);
      m_ub = m_ub == MAX ? m_ub : clamped_add(m_ub, that.m_ub);
    }
    return *this;
  }

  IntervalDomain& operator+=(Num b) { return *this += {b, b}; }

  IntervalDomain operator+(const IntervalDomain& that) const {
    auto cpy = *this;
    cpy += that;
    return cpy;
  }

  bool is_bottom() const override { return m_lb > m_ub; }
  bool is_top() const override { return m_lb == MIN && m_ub == MAX; }

  bool leq(const IntervalDomain& that) const override {
    return is_bottom() || (that.m_lb <= m_lb && m_ub <= that.m_ub);
  }

  bool equals(const IntervalDomain& that) const override {
    return m_lb == that.m_lb && m_ub == that.m_ub;
  }

  void set_to_bottom() override {
    m_lb = MAX;
    m_ub = MIN;
  }

  void set_to_top() override {
    m_lb = MIN;
    m_ub = MAX;
  }

  /*
   *    _|_  \/ [a,b] = [a, b]
   *   [a,b] \/  _|_  = [a, b]
   *   [a,b] \/ [c,d] = [min(a,c), max(b,d)]
   */
  void join_with(const IntervalDomain& that) override {
    m_lb = std::min(m_lb, that.m_lb);
    m_ub = std::max(m_ub, that.m_ub);
  }

  /*
   *    _|_  W [a,b] = [a, b]
   *   [a,b] W  _|_  = [a, b]
   *   [a,b] W [c,d] = [ c < a ? -inf : a
   *                   , b < d ? +inf : b]
   */
  void widen_with(const IntervalDomain& that) override {
    if (is_bottom()) {
      *this = that;
      return;
    }

    if (that.m_lb < m_lb) {
      m_lb = MIN;
    }

    if (m_ub < that.m_ub) {
      m_ub = MAX;
    }
  }

  /*
   *   _|_  /\   _   = _|_
   *    _   /\  _|_  = _|_
   *  [a,b] /\ [c,d] = [max(a,c), min(b,d)]
   */
  void meet_with(const IntervalDomain& that) override {
    m_lb = std::max(m_lb, that.m_lb);
    m_ub = std::min(m_ub, that.m_ub);

    if (is_bottom()) {
      // Normalize the representation of bottom to simplify equality.
      set_to_bottom();
    }
  }

  /*
   *    _|_  N   _   = _|_
   *     _   N  _|_  = _|_
   *   [a,b] N [c,d] = [ a == -inf ? c : a
   *                   , b == +inf ? d : b]
   */
  void narrow_with(const IntervalDomain& that) override {
    if (that.is_bottom()) {
      set_to_bottom();
      return;
    }

    if (m_lb == MIN) {
      m_lb = that.m_lb;
    }

    if (m_ub == MAX) {
      m_ub = that.m_ub;
    }

    if (is_bottom()) {
      // Normalize the representation of bottom to simplify equality.
      set_to_bottom();
    }
  }

 private:
  Num m_lb;
  Num m_ub;

  IntervalDomain(Num lb, Num ub) : m_lb(lb), m_ub(ub) {}

  /*
   * Addition with overflow and underflow protection.
   */
  static Num clamped_add(Num a, Num b) {
    // a + b > MAX
    if (a > 0 && b > MAX - a) {
      return MAX;
    }

    // a + b < MIN
    if (a < 0 && b < MIN - a) {
      return MIN;
    }

    return a + b;
  }
};

template <typename Num>
inline std::ostream& operator<<(std::ostream& o,
                                const sparta::IntervalDomain<Num>& i) {
  if (i.is_bottom()) {
    return o << "_|_";
  }

  if (i.is_top()) {
    return o << "T";
  }

  o << "[";
  if (i.lower_bound() == sparta::IntervalDomain<Num>::MIN) {
    o << "-inf";
  } else {
    // Unary plus promotes types narrower than `int` to be at least `int`
    // avoiding the ostream interpreting them as characters.
    o << +i.lower_bound();
  }

  o << ", ";
  if (i.upper_bound() == sparta::IntervalDomain<Num>::MAX) {
    o << "+inf";
  } else {
    o << +i.upper_bound();
  }

  return o << "]";
}

} // namespace sparta
