/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace sparta {

/**
 * A set of enum values.
 *
 * `EnumBitSet<Enum>` can be used to store an OR-combination of enum values,
 * where `Enum` is an enum class type.
 *
 * `Enum` underlying values must be unsigned integers between 0 and 63. `Enum`
 * must have a `_Count` member containing the maximum value.
 */
template <typename Enum>
class EnumBitSet final {
 private:
  static_assert(std::is_enum_v<Enum>, "Enum must be an enumeration type");
  static_assert(
      std::is_unsigned_v<std::underlying_type_t<Enum>>,
      "The underlying type of Enum must be an unsigned arithmetic type");
  static_assert(static_cast<std::underlying_type_t<Enum>>(Enum::_Count) < 64u,
                "Enum::_Count must be less than 64");

 public:
  using EnumType = Enum;
  using EnumUnderlyingT = std::underlying_type_t<Enum>;

 private:
  using IntT = std::conditional_t<
      static_cast<EnumUnderlyingT>(Enum::_Count) < 8u,
      std::uint8_t,
      std::conditional_t<static_cast<EnumUnderlyingT>(Enum::_Count) < 32u,
                         std::uint32_t,
                         std::uint64_t>>;

  static constexpr IntT enum_to_bit(Enum value) {
    return static_cast<IntT>(1)
           << static_cast<IntT>(static_cast<EnumUnderlyingT>(value));
  }

 public:
  EnumBitSet() = default;

  /* implicit */ constexpr EnumBitSet(Enum value)
      : value_(enum_to_bit(value)) {}

  /* implicit */ constexpr EnumBitSet(std::initializer_list<Enum> set)
      : value_(0u) {
    for (auto value : set) {
      value_ |= enum_to_bit(value);
    }
  }

  constexpr EnumBitSet& operator&=(Enum value) {
    value_ &= enum_to_bit(value);
    return *this;
  }

  constexpr EnumBitSet& operator&=(EnumBitSet set) {
    value_ &= set.value_;
    return *this;
  }

  constexpr EnumBitSet& operator|=(Enum value) {
    value_ |= enum_to_bit(value);
    return *this;
  }

  constexpr EnumBitSet& operator|=(EnumBitSet set) {
    value_ |= set.value_;
    return *this;
  }

  constexpr EnumBitSet& operator^=(Enum value) {
    value_ ^= enum_to_bit(value);
    return *this;
  }

  constexpr EnumBitSet& operator^=(EnumBitSet set) {
    value_ ^= set.value_;
    return *this;
  }

  constexpr EnumBitSet operator&(Enum value) const {
    return EnumBitSet(value_ & enum_to_bit(value));
  }

  constexpr EnumBitSet operator&(EnumBitSet set) const {
    return EnumBitSet(value_ & set.value_);
  }

  constexpr EnumBitSet operator|(Enum value) const {
    return EnumBitSet(value_ | enum_to_bit(value));
  }

  constexpr EnumBitSet operator|(EnumBitSet set) const {
    return EnumBitSet(value_ | set.value_);
  }

  constexpr EnumBitSet operator^(Enum value) const {
    return EnumBitSet(value_ ^ enum_to_bit(value));
  }

  constexpr EnumBitSet operator^(EnumBitSet set) const {
    return EnumBitSet(value_ ^ set.value_);
  }

  constexpr EnumBitSet operator~() const { return EnumBitSet(~value_); }

  explicit constexpr operator bool() const { return value_ != 0; }

  constexpr bool operator!() const { return value_ == 0; }

  constexpr bool operator==(EnumBitSet set) const {
    return value_ == set.value_;
  }

  constexpr bool operator!=(EnumBitSet set) const {
    return value_ != set.value_;
  }

  constexpr bool test(Enum value) const {
    return (value_ & enum_to_bit(value)) == enum_to_bit(value);
  }

  constexpr EnumBitSet& set(Enum value, bool on = true) {
    if (on) {
      value_ |= enum_to_bit(value);
    } else {
      value_ &= ~enum_to_bit(value);
    }
    return *this;
  }

  constexpr bool empty() const { return value_ == 0; }

  constexpr void clear() { value_ = 0; }

  constexpr bool is_subset_of(EnumBitSet set) const {
    return (value_ | set.value_) == set.value_;
  }

  constexpr bool has_single_bit() const {
    return (value_ && !(value_ & (value_ - 1)));
  }

 private:
  explicit constexpr EnumBitSet(IntT value) : value_(value) {}

 private:
  IntT value_ = 0;
};

} // namespace sparta
