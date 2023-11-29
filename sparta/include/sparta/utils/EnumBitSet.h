/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>
#include <type_traits>

namespace sparta {

/**
 * A set of enum values.
 *
 * `EnumBitSet<Enum>` can be used to store an OR-combination of enum values,
 * where `Enum` is an enum class type. `Enum` underlying values must be a power
 * of 2.
 */
template <typename Enum>
class EnumBitSet final {
  static_assert(std::is_enum_v<Enum>, "Enum must be an enumeration type");
  static_assert(
      std::is_unsigned_v<std::underlying_type_t<Enum>>,
      "The underlying type of Enum must be an unsigned arithmetic type");

 public:
  using EnumType = Enum;
  using IntT = std::underlying_type_t<Enum>;

 public:
  EnumBitSet() = default;

  /* implicit */ constexpr EnumBitSet(Enum value)
      : value_(static_cast<IntT>(value)) {}

  /* implicit */ constexpr EnumBitSet(std::initializer_list<Enum> set)
      : value_(0) {
    for (auto value : set) {
      value_ |= static_cast<IntT>(value);
    }
  }

  constexpr EnumBitSet& operator&=(Enum value) {
    value_ &= static_cast<IntT>(value);
    return *this;
  }

  constexpr EnumBitSet& operator&=(EnumBitSet set) {
    value_ &= set.value_;
    return *this;
  }

  constexpr EnumBitSet& operator|=(Enum value) {
    value_ |= static_cast<IntT>(value);
    return *this;
  }

  constexpr EnumBitSet& operator|=(EnumBitSet set) {
    value_ |= set.value_;
    return *this;
  }

  constexpr EnumBitSet& operator^=(Enum value) {
    value_ ^= static_cast<IntT>(value);
    return *this;
  }

  constexpr EnumBitSet& operator^=(EnumBitSet set) {
    value_ ^= set.value_;
    return *this;
  }

  constexpr EnumBitSet operator&(Enum value) const {
    return EnumBitSet(value_ & static_cast<IntT>(value));
  }

  constexpr EnumBitSet operator&(EnumBitSet set) const {
    return EnumBitSet(value_ & set.value_);
  }

  constexpr EnumBitSet operator|(Enum value) const {
    return EnumBitSet(value_ | static_cast<IntT>(value));
  }

  constexpr EnumBitSet operator|(EnumBitSet set) const {
    return EnumBitSet(value_ | set.value_);
  }

  constexpr EnumBitSet operator^(Enum value) const {
    return EnumBitSet(value_ ^ static_cast<IntT>(value));
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
    if (static_cast<IntT>(value) == 0) {
      return value_ == 0;
    } else {
      return (value_ & static_cast<IntT>(value)) == static_cast<IntT>(value);
    }
  }

  constexpr EnumBitSet& set(Enum value, bool on = true) {
    if (on) {
      value_ |= static_cast<IntT>(value);
    } else {
      value_ &= ~static_cast<IntT>(value);
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

  constexpr IntT encode() const { return value_; }

  static constexpr EnumBitSet decode(IntT encoding) {
    return EnumBitSet(encoding);
  }

 private:
  explicit constexpr EnumBitSet(IntT value) : value_(value) {}

 private:
  IntT value_ = 0;
};

} // namespace sparta
