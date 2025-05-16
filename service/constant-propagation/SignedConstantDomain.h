/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>
#include <limits>
#include <optional>

#include <sparta/AbstractDomain.h>
#include <sparta/ConstantAbstractDomain.h>
#include <sparta/IntervalDomain.h>

#include "Debug.h"
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
  struct Bounds final {
    bool is_nez;
    int64_t l;
    int64_t u;
    bool operator==(const Bounds& other) const {
      return is_nez == other.is_nez && l == other.l && u == other.u;
    }
    bool operator<=(const Bounds& other) const {
      return this->is_bottom() ||
             (other.l <= l && u <= other.u && other.is_nez <= is_nez);
    }
    // Partial order only. Use <= instead.
    bool operator>(const Bounds&) = delete;
    bool operator<(const Bounds&) = delete;
    bool operator>=(const Bounds&) = delete;

    inline bool is_constant() const { return l == u; }
    bool is_top() const { return *this == top(); }
    bool is_bottom() const { return *this == bottom(); }
    void normalize() {
      if (is_nez) {
        if (l == 0) l++;
        if (u == 0) u--;
      }
      if (u < l) this->set_to_bottom();
      always_assert(is_normalized());
    }
    // Is the constant not within the bounds?
    bool unequals_constant(int64_t integer) const {
      return (integer == 0 && is_nez) || integer < l || u < integer;
    }
    bool is_normalized() const {
      // bottom has a particular shape
      if (u < l) return this->is_bottom();
      // nez cannot be set if 0 is a lower or upper bound
      if (l == 0 || u == 0) return !is_nez;
      // nez must be set if 0 is not in range
      return (l <= 0 && u >= 0) || is_nez;
    }

    Bounds& set_to_top() {
      *this = top();
      return *this;
    }

    Bounds& set_to_bottom() {
      *this = bottom();
      return *this;
    }

    Bounds& join_with(const Bounds& that) {
      l = std::min(l, that.l);
      u = std::max(u, that.u);
      is_nez &= that.is_nez;
      always_assert(is_normalized());
      return *this;
    }

    Bounds& meet_with(const Bounds& that) {
      l = std::max(l, that.l);
      u = std::min(u, that.u);
      is_nez |= that.is_nez;
      normalize();
      return *this;
    }

    static inline Bounds from_interval(sign_domain::Interval interval) {
      switch (interval) {
      case sign_domain::Interval::EMPTY:
        return bottom();
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
        return top();
      case sign_domain::Interval::NEZ:
        return nez();
      case sign_domain::Interval::SIZE:
        not_reached();
      }
    }

    static Bounds from_integer(int64_t integer) {
      return Bounds{integer != 0, integer, integer};
    }

    static const Bounds& top() {
      static const Bounds res{false, MIN, MAX};
      return res;
    }
    static const Bounds& bottom() {
      static const Bounds res{true, MAX, MIN};
      return res;
    }
    static const Bounds& nez() {
      static const Bounds res{true, MIN, MAX};
      return res;
    }
  };
  Bounds m_bounds;

  class Bitset final {
    // Albeit unusual, make sure the compiler uses 2's complement to represent
    // int64, upon which much of this class relies.
    static_assert(
        static_cast<uint64_t>(static_cast<int64_t>(-1)) ==
                std::numeric_limits<uint64_t>::max() &&
            static_cast<int64_t>(std::numeric_limits<uint64_t>::max()) ==
                static_cast<int64_t>(-1),
        "Unsupported compiler: int64_t is not represented as 2's complement");

    // We use two integers to represent the state of each bit. A bit of
    // one_bit_states/zero_bit_states being one means that the corresponding bit
    // of the integer can possibly be one/zero. Hence, if the same bits of both
    // are one, it means that the bit can be either one or zero, i.e., top for
    // that bit. If any bit is zero in both integers, then the bitset is bottom.
    // Use uint64_t instead of int64_t to avoid undefined behavior with >>.
    //
    // For 32-bit integers, the high 32 bits should be the same as the highest
    // bit of the lower 32 bits, i.e., the sign bit of the integer. In this way,
    // we achieve consistency with SignedConstantDomain initialized from a
    // constant.
    uint64_t one_bit_states{std::numeric_limits<uint64_t>::max()};
    uint64_t zero_bit_states{std::numeric_limits<uint64_t>::max()};

    void set_all_to(bool zero, bool one) {
      zero_bit_states = zero ? std::numeric_limits<uint64_t>::max() : 0u;
      one_bit_states = one ? std::numeric_limits<uint64_t>::max() : 0u;
    }

    // Construct with all bits set to a given bit state.
    Bitset(bool zero, bool one) { set_all_to(zero, one); }

   public:
    Bitset() = default;
    Bitset(const Bitset& that) = default;
    Bitset(Bitset&& that) = default;
    Bitset& operator=(const Bitset& that) = default;
    Bitset& operator=(Bitset&& that) = default;

    bool operator==(const Bitset& that) const {
      return (one_bit_states == that.one_bit_states &&
              zero_bit_states == that.zero_bit_states) ||
             (is_bottom() && that.is_bottom());
    }

    bool operator<=(const Bitset& that) const {
      if (is_bottom()) return true;
      return ((one_bit_states | that.one_bit_states) == that.one_bit_states &&
              (zero_bit_states | that.zero_bit_states) == that.zero_bit_states);
    }

    // Partial order only. Use <= instead.
    bool operator>(const Bitset&) = delete;
    bool operator<(const Bitset&) = delete;
    bool operator>=(const Bitset&) = delete;

    uint64_t get_one_bit_states() const { return one_bit_states; }
    uint64_t get_zero_bit_states() const { return zero_bit_states; }

    // Construct from a constant.
    explicit Bitset(int64_t value) {
      one_bit_states = static_cast<uint64_t>(value);
      zero_bit_states = ~one_bit_states;
    }

    bool is_constant() const { return get_constant().has_value(); }

    std::optional<int64_t> get_constant() const {
      if (~one_bit_states == zero_bit_states) {
        return static_cast<int64_t>(one_bit_states);
      } else {
        return std::nullopt;
      }
    }

    void set_to_bottom() { set_all_to(false, false); }
    void set_to_top() { set_all_to(true, true); }
    Bitset& join_with(const Bitset& that) {
      one_bit_states |= that.one_bit_states;
      zero_bit_states |= that.zero_bit_states;
      return *this;
    }

    Bitset& meet_with(const Bitset& that) {
      one_bit_states &= that.one_bit_states;
      zero_bit_states &= that.zero_bit_states;
      return *this;
    }

    bool is_bottom() const {
      // We don't use a single representation for bottom. Always use this
      // function to check if it's bottom. It's bottom if any bit is zero in
      // both integers.
      return (one_bit_states | zero_bit_states) !=
             std::numeric_limits<uint64_t>::max();
    }

    bool is_top() const { return *this == top(); }

    // Get the bit that can be determined to be one or zero. The returned
    // integer hosts all bits that are determined to be zero/one.
    uint64_t get_determined_zero_bits() const {
      return zero_bit_states & ~one_bit_states;
    }
    uint64_t get_determined_one_bits() const {
      return one_bit_states & ~zero_bit_states;
    }

    // Set particular bits to be known to be zero/one.
    Bitset& set_determined_zero_bits(uint64_t bits) {
      one_bit_states &= ~bits;
      zero_bit_states |= bits;
      return *this;
    }
    Bitset& set_determined_one_bits(uint64_t bits) {
      one_bit_states |= bits;
      zero_bit_states &= ~bits;
      return *this;
    }

    // Is the constant unrepresentable by the bitset?
    bool unequals_constant(int64_t integer) const {
      const auto determinable_one_bits = get_determined_one_bits();
      if ((determinable_one_bits & integer) != determinable_one_bits) {
        return true;
      }

      const auto determinable_zero_bits = get_determined_zero_bits();
      if ((determinable_zero_bits & ~integer) != determinable_zero_bits) {
        return true;
      }

      return false;
    }

    static const Bitset& bottom() {
      static const Bitset res(false, false);
      return res;
    }
    static const Bitset& top() {
      static const Bitset res(true, true);
      return res;
    }
  };

  Bitset m_bitset;

  SignedConstantDomain(Bounds bounds, Bitset bitset)
      : m_bounds(bounds), m_bitset(bitset) {}

  // When either bounds or bitset meets (become narrower), we can possibly infer
  // the other one with some info.
  void cross_infer_meet_from_bounds() {
    if (m_bitset.is_bottom()) {
      always_assert(m_bounds.is_bottom());
      return;
    }

    // Constant inference
    if (m_bounds.is_constant()) {
      if (m_bitset.unequals_constant(m_bounds.l)) {
        set_to_bottom();
        return;
      }
      m_bitset = Bitset(m_bounds.l);
      return;
    }

    // One is bottom, then all is bottom.
    if (m_bounds.is_bottom()) {
      set_to_bottom();
      return;
    }

    // TODO(T222824773) More cross inference can be added here...
  }

  void cross_infer_meet_from_bitset() {
    if (m_bounds.is_bottom()) {
      always_assert(m_bitset.is_bottom());
      return;
    }

    const auto bitset_constant = m_bitset.get_constant();

    if (bitset_constant.has_value()) {
      if (m_bounds.unequals_constant(bitset_constant.value())) {
        set_to_bottom();
        return;
      }
      m_bounds = Bounds::from_integer(bitset_constant.value());
      return;
    }

    // One is bottom, then all is bottom.
    if (m_bitset.is_bottom()) {
      set_to_bottom();
      return;
    }

    // TODO(T222824773) More cross inference can be added here...
  }

 public:
  SignedConstantDomain() : m_bounds(Bounds::top()), m_bitset(Bitset::top()) {}

  explicit SignedConstantDomain(int64_t v)
      : m_bounds{Bounds::from_integer(v)}, m_bitset(v) {}

  explicit SignedConstantDomain(sign_domain::Interval interval)
      : m_bounds(Bounds::from_interval(interval)), m_bitset(Bitset::top()) {
    cross_infer_meet_from_bounds();
  }

  SignedConstantDomain(int64_t min, int64_t max)
      : m_bounds({min > 0 || max < 0, min, max}), m_bitset(Bitset::top()) {
    always_assert(min <= max);
    cross_infer_meet_from_bounds();
  }

  // Construct a SignedConstantDomain that is the joint of multiple constants.
  static SignedConstantDomain from_constants(
      std::initializer_list<int64_t> constants) {
    SignedConstantDomain scd(bottom());
    for (const auto c : constants) {
      scd.join_with(SignedConstantDomain(c));
    }
    return scd;
  }

  static SignedConstantDomain bottom() {
    return SignedConstantDomain(Bounds::bottom(), Bitset::bottom());
  }
  static SignedConstantDomain top() {
    return SignedConstantDomain(Bounds::top(), Bitset::top());
  }
  static SignedConstantDomain nez() {
    return SignedConstantDomain(Bounds::nez(), Bitset::top());
  }
  bool is_bottom() const {
    return m_bounds.is_bottom() || m_bitset.is_bottom();
  }
  bool is_top() const { return m_bounds.is_top() && m_bitset.is_top(); }
  bool is_nez() const { return m_bounds.is_nez; }

  bool leq(const SignedConstantDomain& that) const {
    return m_bounds <= that.m_bounds && m_bitset <= that.m_bitset;
  }

  bool equals(const SignedConstantDomain& that) const {
    return m_bounds == that.m_bounds && m_bitset == that.m_bitset;
  }

  void set_to_bottom() {
    m_bounds.set_to_bottom();
    m_bitset.set_to_bottom();
  }

  void set_to_top() {
    m_bounds.set_to_top();
    m_bitset.set_to_top();
  }

  void join_with(const SignedConstantDomain& that) {
    m_bounds.join_with(that.m_bounds);
    m_bitset.join_with(that.m_bitset);
  }

  void widen_with(const SignedConstantDomain&) {
    throw std::runtime_error("widen_with not implemented!");
  }

  void meet_with(const SignedConstantDomain& that) {
    m_bounds.meet_with(that.m_bounds);
    cross_infer_meet_from_bounds();
    m_bitset.meet_with(that.m_bitset);
    cross_infer_meet_from_bitset();
  }

  void narrow_with(const SignedConstantDomain&) {
    throw std::runtime_error("narrow_with not implemented!");
  }

  sign_domain::Domain interval_domain() const {
    return sign_domain::Domain(interval());
  }

  sign_domain::Interval interval() const {
    if (m_bounds.is_bottom()) return sign_domain::Interval::EMPTY;
    if (m_bounds.l > 0) return sign_domain::Interval::GTZ;
    if (m_bounds.u < 0) return sign_domain::Interval::LTZ;
    if (m_bounds.l == 0) {
      if (m_bounds.u == 0) return sign_domain::Interval::EQZ;
      return sign_domain::Interval::GEZ;
    }
    if (m_bounds.u == 0) return sign_domain::Interval::LEZ;
    if (m_bounds.is_nez) return sign_domain::Interval::NEZ;
    return sign_domain::Interval::ALL;
  }

  ConstantDomain constant_domain() const {
    if (const auto constant = get_constant(); constant.has_value()) {
      return ConstantDomain(*constant);
    }
    if (is_bottom()) return ConstantDomain::bottom();
    return ConstantDomain::top();
  }

  NumericIntervalDomain numeric_interval_domain() const {
    if (m_bounds.is_bottom()) return NumericIntervalDomain::bottom();
    if (m_bounds == Bounds::nez()) return NumericIntervalDomain::top();
    return numeric_interval_domain_from_int(m_bounds.l, m_bounds.u);
  }

  boost::optional<int64_t> get_constant() const {
    if (!m_bounds.is_constant()) return boost::none;
    always_assert(m_bitset.is_constant() &&
                  *m_bitset.get_constant() == m_bounds.l);
    return boost::optional<int64_t>(m_bounds.l);
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

  uint64_t get_determined_zero_bits() const {
    return m_bitset.get_determined_zero_bits();
  }

  uint64_t get_determined_one_bits() const {
    return m_bitset.get_determined_one_bits();
  }

  // Sets determined bits. This also wipes out any inference about bounds by
  // setting bounds to top if either zeros or ones is provided. Useful in
  // inferring results of bitwise ops, which usually invalidate any existing
  // inferences on Bounds.
  SignedConstantDomain& set_determined_bits_erasing_bounds(
      std::optional<uint64_t> zeros, std::optional<uint64_t> ones, bool bit32) {
    // No bit can be 1 in both zeros and ones
    always_assert(!zeros.has_value() || !ones.has_value() ||
                  (*ones & *zeros) == 0u);

    if (!zeros.has_value() && !ones.has_value()) {
      return *this;
    }

    if (zeros.has_value()) {
      uint64_t new_zeros = *zeros;
      if (bit32) {
        if ((*zeros & 0x80000000) != 0) { // sign bit is 1
          new_zeros |= static_cast<uint64_t>(0xffffffff00000000ul);
        } else {
          new_zeros &= static_cast<uint64_t>(0x7ffffffful);
        }
      }
      m_bitset.set_determined_zero_bits(new_zeros);
    }
    if (ones.has_value()) {
      uint64_t new_ones = *ones;
      if (bit32) {
        if ((*ones & 0x80000000) != 0) { // sign bit is 1
          new_ones |= static_cast<uint64_t>(0xffffffff00000000ul);
        } else {
          new_ones &= static_cast<uint64_t>(0x7ffffffful);
        }
      }
      m_bitset.set_determined_one_bits(new_ones);
    }

    m_bounds.set_to_top();
    cross_infer_meet_from_bitset();
    return *this;
  }

  uint64_t get_one_bit_states() const { return m_bitset.get_one_bit_states(); }
  uint64_t get_zero_bit_states() const {
    return m_bitset.get_zero_bit_states();
  }

  SignedConstantDomain& left_shift_bits_int(int32_t shift) {
    if (is_bottom()) return *this;

    shift &= BIT_SHIFT_MASK_INT;
    // The higher 32 bits must be cleaned up, otherwise int meet may lead to
    // unintended bottoms due to mismatch in the higher 32 bits.
    // set_determined_bits_erasing_bounds() does not reset existing bit states.
    // Set to top first to clear bit states.
    uint64_t new_determined_zeros =
        ((~((~get_determined_zero_bits()) << shift)) & 0xffffffff);
    uint64_t new_determined_ones =
        (get_determined_one_bits() << shift) & 0xffffffff;
    const bool sign_zero = (new_determined_zeros & (1ul << 31)) != 0;
    const bool sign_one = (new_determined_ones & (1ul << 31)) != 0;
    always_assert_log(!sign_zero || !sign_one,
                      "When left shifting, the sign can't be determined to "
                      "both zero and one");
    if (sign_zero) {
      new_determined_zeros |= 0xffffffff00000000ul;
    } else if (sign_one) {
      new_determined_ones |= 0xffffffff00000000ul;
    }
    set_to_top();
    set_determined_bits_erasing_bounds(new_determined_zeros,
                                       new_determined_ones, /*bit32=*/true);
    return *this;
  }
  SignedConstantDomain& left_shift_bits_long(int32_t shift) {
    if (is_bottom()) return *this;

    shift &= BIT_SHIFT_MASK_LONG;
    // set_determined_bits_erasing_bounds() does not reset existing bit states.
    // Set to top first to clear bit states.
    const uint64_t new_determined_zeros =
        (~((~get_determined_zero_bits()) << shift));
    const uint64_t new_determined_ones = get_determined_one_bits() << shift;
    set_to_top();
    set_determined_bits_erasing_bounds(new_determined_zeros,
                                       new_determined_ones, /*bit32=*/false);
    return *this;
  }

  SignedConstantDomain& unsigned_right_shift_bits_int(int32_t shift) {
    if (is_bottom()) return *this;

    shift &= BIT_SHIFT_MASK_INT;

    // set_determined_bits_erasing_bounds() does not reset existing bit states.
    // Set to top first to clear bit states.
    const uint64_t new_determined_zeros =
        ~((static_cast<uint32_t>(~get_determined_zero_bits())) >> shift);
    const uint64_t new_determined_ones =
        static_cast<uint32_t>(get_determined_one_bits()) >> shift;
    set_to_top();
    set_determined_bits_erasing_bounds(new_determined_zeros,
                                       new_determined_ones,
                                       /*bit32=*/true);
    return *this;
  }
  SignedConstantDomain& unsigned_right_shift_bits_long(int32_t shift) {
    if (is_bottom()) return *this;

    shift &= BIT_SHIFT_MASK_LONG;

    // set_determined_bits_erasing_bounds() does not reset existing bit states.
    // Set to top first to clear bit states.
    const uint64_t new_determined_zeros =
        ~((~get_determined_zero_bits()) >> shift);
    const uint64_t new_determined_ones = get_determined_one_bits() >> shift;
    set_to_top();
    set_determined_bits_erasing_bounds(new_determined_zeros,
                                       new_determined_ones,
                                       /*bit32=*/false);
    return *this;
  }
  // TODO(T222824773) Add signed right shift, which has implications on bounds.

 private:
  static constexpr int32_t BIT_SHIFT_MASK_INT = 0x1f;
  static constexpr int32_t BIT_SHIFT_MASK_LONG = 0x3f;

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
  if (scd.is_top()) return o << "T";

  const auto print_bounds =
      [](std::ostream& o, const SignedConstantDomain& scd) -> std::ostream& {
    const auto min = scd.min_element();
    const auto max = scd.max_element();
    if (min == std::numeric_limits<int64_t>::min() &&
        max == std::numeric_limits<int64_t>::max()) {
      return o << (scd.is_nez() ? "NEZ" : "TB");
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
  };

  print_bounds(o, scd);

  const auto print_bitset =
      [](std::ostream& o, const SignedConstantDomain& scd) -> std::ostream& {
    const auto min_max = SignedConstantDomain::from_constants(
        {scd.min_element(), scd.max_element()});
    if (min_max.get_zero_bit_states() == scd.get_zero_bit_states() &&
        min_max.get_one_bit_states() == scd.get_one_bit_states()) {
      // No interesting bitset info.
      return o;
    }

    std::ios old_io_state(nullptr);
    old_io_state.copyfmt(o);
    o << "{" << std::hex << std::showbase << scd.get_zero_bit_states() << "/"
      << scd.get_one_bit_states() << "}";
    o.copyfmt(old_io_state);
    return o;
  };

  return print_bitset(o, scd);
}
