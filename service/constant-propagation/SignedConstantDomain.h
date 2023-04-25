/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "AbstractDomain.h"
#include "ConstantAbstractDomain.h"
#include "IntervalDomain.h"
#include "SignDomain.h"

using ConstantDomain = sparta::ConstantAbstractDomain<int64_t>;
using NumericIntervalType = int64_t;
using NumericIntervalDomain = sparta::IntervalDomain<NumericIntervalType>;

// input interval must not be empty
inline NumericIntervalDomain numeric_interval_domain_from_int(int64_t min,
                                                              int64_t max) {
  always_assert(min <= max);
  if (min <= NumericIntervalDomain::MIN) {
    if (max >= NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::top();
    } else if (max > NumericIntervalDomain::MIN) {
      return NumericIntervalDomain::bounded_above(max);
    } else {
      return NumericIntervalDomain::low();
    }
  } else {
    if (max < NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::finite(min, max);
    } else if (min < NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::bounded_below(min);
    } else {
      return NumericIntervalDomain::high();
    }
  }
}

// This class effectively implements a
//   ReducedProductAbstractDomain<SignedConstantDomain,
//     sign_domain::Domain, NumericIntervalDomain, ConstantDomain>
class SignedConstantDomain final
    : public sparta::AbstractDomain<SignedConstantDomain> {
 private:
  static constexpr int64_t MIN = std::numeric_limits<int64_t>::min();
  static constexpr int64_t MAX = std::numeric_limits<int64_t>::max();
  struct Bounds {
    bool nez;
    int64_t l;
    int64_t u;
    bool operator==(const Bounds& other) const {
      return nez == other.nez && l == other.l && u == other.u;
    }
    inline bool is_constant() const { return l == u; }
    void normalize() {
      if (nez) {
        if (l == 0) l++;
        if (u == 0) u--;
      }
      if (u < l) *this = BOTTOM;
      always_assert(is_normalized());
    }
    bool is_normalized() const {
      // bottom has a particular shape
      if (u < l) return *this == BOTTOM;
      // nez cannot be set if 0 is a lower or upper bound
      if (l == 0 || u == 0) return !nez;
      // nez must be set if 0 is not in range
      return (l <= 0 && u >= 0) || nez;
    }
    static inline Bounds from_interval(sign_domain::Interval interval) {
      switch (interval) {
      case sign_domain::Interval::EMPTY:
        return BOTTOM;
      case sign_domain::Interval::EQZ:
        return {false, 0, 0};
      case sign_domain::Interval::LEZ:
        return {false, MIN, 0};
      case sign_domain::Interval::LTZ:
        return {true, MIN, -1};
      case sign_domain::Interval::GEZ:
        return {false, 0, MAX};
      case sign_domain::Interval::GTZ:
        return {true, 1, MAX};
      case sign_domain::Interval::ALL:
        return TOP;
      case sign_domain::Interval::NEZ:
        return NEZ;
      case sign_domain::Interval::SIZE:
        not_reached();
      }
    }
  };
  static constexpr Bounds TOP = {false, MIN, MAX};
  static constexpr Bounds BOTTOM = {true, MAX, MIN};
  static constexpr Bounds NEZ = {true, MIN, MAX};
  Bounds m_bounds;

  explicit SignedConstantDomain(Bounds bounds) : m_bounds(bounds) {}

 public:
  SignedConstantDomain() : m_bounds(TOP) {}

  explicit SignedConstantDomain(int64_t v) : m_bounds{v != 0, v, v} {}

  explicit SignedConstantDomain(sign_domain::Interval interval)
      : m_bounds(Bounds::from_interval(interval)) {}

  SignedConstantDomain(int64_t min, int64_t max)
      : m_bounds({min > 0 || max < 0, min, max}) {
    always_assert(min <= max);
  }

  static SignedConstantDomain bottom() { return SignedConstantDomain(BOTTOM); }
  static SignedConstantDomain top() { return SignedConstantDomain(TOP); }
  static SignedConstantDomain nez() { return SignedConstantDomain(NEZ); }
  bool is_bottom() const { return m_bounds == BOTTOM; }
  bool is_top() const { return m_bounds == TOP; }
  bool is_nez() const { return m_bounds.nez; }

  bool leq(const SignedConstantDomain& that) const {
    if (m_bounds == BOTTOM) return true;
    return that.m_bounds.l <= m_bounds.l && m_bounds.u <= that.m_bounds.u &&
           that.m_bounds.nez <= m_bounds.nez;
  }

  bool equals(const SignedConstantDomain& that) const {
    return m_bounds == that.m_bounds;
  }

  void set_to_bottom() { m_bounds = BOTTOM; }

  void set_to_top() { m_bounds = TOP; }

  void join_with(const SignedConstantDomain& that) {
    m_bounds.l = std::min(m_bounds.l, that.m_bounds.l);
    m_bounds.u = std::max(m_bounds.u, that.m_bounds.u);
    m_bounds.nez &= that.m_bounds.nez;
    always_assert(m_bounds.is_normalized());
  }

  void widen_with(const SignedConstantDomain&) {
    throw std::runtime_error("widen_with not implemented!");
  }

  void meet_with(const SignedConstantDomain& that) {
    m_bounds.l = std::max(m_bounds.l, that.m_bounds.l);
    m_bounds.u = std::min(m_bounds.u, that.m_bounds.u);
    m_bounds.nez |= that.m_bounds.nez;
    m_bounds.normalize();
  }

  void narrow_with(const SignedConstantDomain&) {
    throw std::runtime_error("narrow_with not implemented!");
  }

  sign_domain::Domain interval_domain() const {
    return sign_domain::Domain(interval());
  }

  sign_domain::Interval interval() const {
    if (m_bounds == BOTTOM) return sign_domain::Interval::EMPTY;
    if (m_bounds.l > 0) return sign_domain::Interval::GTZ;
    if (m_bounds.u < 0) return sign_domain::Interval::LTZ;
    if (m_bounds.l == 0) {
      if (m_bounds.u == 0) return sign_domain::Interval::EQZ;
      return sign_domain::Interval::GEZ;
    }
    if (m_bounds.u == 0) return sign_domain::Interval::LEZ;
    if (m_bounds.nez) return sign_domain::Interval::NEZ;
    return sign_domain::Interval::ALL;
  }

  ConstantDomain constant_domain() const {
    if (!m_bounds.is_constant()) {
      if (m_bounds == BOTTOM) return ConstantDomain::bottom();
      return ConstantDomain::top();
    }
    return ConstantDomain(m_bounds.l);
  }

  NumericIntervalDomain numeric_interval_domain() const {
    if (m_bounds == BOTTOM) return NumericIntervalDomain::bottom();
    if (m_bounds == NEZ) return NumericIntervalDomain::top();
    return numeric_interval_domain_from_int(m_bounds.l, m_bounds.u);
  }

  boost::optional<int64_t> get_constant() const {
    if (!m_bounds.is_constant()) return boost::none;
    return boost::optional<int64_t>(m_bounds.l);
  }

  static SignedConstantDomain default_value() {
    return SignedConstantDomain(0);
  }

  /* Return the largest element within the interval. */
  int64_t max_element() const {
    always_assert(m_bounds.l <= m_bounds.u);
    return m_bounds.u;
  }

  /* Return the smallest element within the interval. */
  int64_t min_element() const {
    always_assert(m_bounds.l <= m_bounds.u);
    return m_bounds.l;
  }

  /* Return the largest element within the interval, clamped to int32_t. */
  int32_t max_element_int() const { return clamp_int(max_element()); }

  /* Return the smallest element within the interval, clamped to int32_t. */
  int32_t min_element_int() const { return clamp_int(min_element()); }

  // Meet with int32_t bounds.
  SignedConstantDomain clamp_int() const {
    auto res = *this;
    res.meet_with(SignedConstantDomain(std::numeric_limits<int32_t>::min(),
                                       std::numeric_limits<int32_t>::max()));
    return res;
  }

 private:
  static int32_t clamp_int(int64_t value) {
    return std::max(
        std::min(value,
                 static_cast<int64_t>(std::numeric_limits<int32_t>::max())),
        static_cast<int64_t>(std::numeric_limits<int32_t>::min()));
  }
};

inline std::ostream& operator<<(std::ostream& o,
                                const SignedConstantDomain& scd) {
  if (scd.is_bottom()) return o << "_|_";

  auto min = scd.min_element();
  auto max = scd.max_element();
  if (min == std::numeric_limits<int64_t>::min() &&
      max == std::numeric_limits<int64_t>::max()) {
    return o << (scd.is_nez() ? "NEZ" : "T");
  }

  if (min == std::numeric_limits<int64_t>::min()) {
    if (max == -1) return o << "LTZ";
    if (max == 0) return o << "LEZ";
  }
  if (max == std::numeric_limits<int64_t>::max()) {
    if (min == 1) return o << "GTZ";
    if (min == 0) return o << "GEZ";
  }

  auto append = [&o](int64_t v) -> std::ostream& {
    if (v == std::numeric_limits<int64_t>::min()) return o << "min";
    if (v == std::numeric_limits<int64_t>::max()) return o << "max";
    return o << v;
  };

  if (min == max) return append(min);

  o << "[";
  append(min);
  if (min < 0 && max > 0 && scd.is_nez()) {
    o << ",-1]U[1,";
  } else {
    o << ",";
  }
  append(max);
  return o << "]";
}
