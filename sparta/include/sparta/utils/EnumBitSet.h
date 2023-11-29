/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <bitset>
#include <initializer_list>
#include <type_traits>

namespace sparta {

/**
 * A set of enum values.
 *
 * `EnumBitSet<Enum>` can be used to store an OR-combination of enum values,
 * where `Enum` is an enum class type.
 *
 * `Enum` underlying values must be unsigned integers. `Enum`
 * must have a `_Count` member containing the maximum value.
 */
template <typename Enum>
class EnumBitSet final {
 private:
  static_assert(std::is_enum_v<Enum>, "Enum must be an enumeration type");
  static_assert(
      std::is_unsigned_v<std::underlying_type_t<Enum>>,
      "The underlying type of Enum must be an unsigned arithmetic type");
  static_assert(static_cast<std::underlying_type_t<Enum>>(Enum::_Count) >= 0,
                "Enum::_Count must contain the maximum value");
  static_assert(
      static_cast<std::underlying_type_t<Enum>>(Enum::_Count) < 512u,
      "Enum::_Count is too large, consider using a sparse set instead");

 public:
  using EnumType = Enum;
  using EnumUnderlyingT = std::underlying_type_t<Enum>;

 private:
  using BitSetT = std::bitset<static_cast<EnumUnderlyingT>(Enum::_Count) + 1>;

  static constexpr BitSetT enum_to_bit(Enum value) {
    return BitSetT{}.set(static_cast<EnumUnderlyingT>(value), true);
  }

 public:
  EnumBitSet() = default;

  /* implicit */ constexpr EnumBitSet(Enum value)
      : value_{enum_to_bit(value)} {}

  /* implicit */ constexpr EnumBitSet(std::initializer_list<Enum> set)
      : value_{} {
    for (auto value : set) {
      value_.set(static_cast<EnumUnderlyingT>(value), true);
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

  explicit operator bool() const { return value_.any(); }

  bool operator!() const { return value_.none(); }

  bool operator==(const EnumBitSet& set) const { return value_ == set.value_; }

  bool operator!=(EnumBitSet set) const { return value_ != set.value_; }

  bool test(Enum value) const {
    return value_[static_cast<EnumUnderlyingT>(value)];
  }

  constexpr EnumBitSet& set(Enum value, bool on = true) {
    value_.set(static_cast<EnumUnderlyingT>(value), on);
    return *this;
  }

  bool empty() const { return value_.none(); }

  constexpr void clear() { value_.reset(); }

  bool is_subset_of(EnumBitSet set) const {
    return (value_ | set.value_) == set.value_;
  }

  bool has_single_bit() const { return value_.count() == 1; }

 private:
  explicit constexpr EnumBitSet(BitSetT value) : value_(std::move(value)) {}

 private:
  BitSetT value_ = 0;
};

} // namespace sparta
