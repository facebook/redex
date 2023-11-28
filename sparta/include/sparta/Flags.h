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
 * A set of flags.
 *
 * `Flags<Enum>` can be used to store an OR-combination of enum values, where
 * `Enum` is an enum class type. `Enum` underlying values must be a power of 2.
 */
template <typename Enum>
class Flags final {
  static_assert(std::is_enum_v<Enum>, "Enum must be an enumeration type");
  static_assert(
      std::is_unsigned_v<std::underlying_type_t<Enum>>,
      "The underlying type of Enum must be an unsigned arithmetic type");

 public:
  using EnumType = Enum;
  using IntT = std::underlying_type_t<Enum>;

 public:
  Flags() = default;

  /* implicit */ constexpr Flags(Enum flag) : value_(static_cast<IntT>(flag)) {}

  /* implicit */ constexpr Flags(std::initializer_list<Enum> flags)
      : value_(0) {
    for (auto flag : flags) {
      value_ |= static_cast<IntT>(flag);
    }
  }

  constexpr Flags& operator&=(Enum flag) {
    value_ &= static_cast<IntT>(flag);
    return *this;
  }

  constexpr Flags& operator&=(Flags flags) {
    value_ &= flags.value_;
    return *this;
  }

  constexpr Flags& operator|=(Enum flag) {
    value_ |= static_cast<IntT>(flag);
    return *this;
  }

  constexpr Flags& operator|=(Flags flags) {
    value_ |= flags.value_;
    return *this;
  }

  constexpr Flags& operator^=(Enum flag) {
    value_ ^= static_cast<IntT>(flag);
    return *this;
  }

  constexpr Flags& operator^=(Flags flags) {
    value_ ^= flags.value_;
    return *this;
  }

  constexpr Flags operator&(Enum flag) const {
    return Flags(value_ & static_cast<IntT>(flag));
  }

  constexpr Flags operator&(Flags flags) const {
    return Flags(value_ & flags.value_);
  }

  constexpr Flags operator|(Enum flag) const {
    return Flags(value_ | static_cast<IntT>(flag));
  }

  constexpr Flags operator|(Flags flags) const {
    return Flags(value_ | flags.value_);
  }

  constexpr Flags operator^(Enum flag) const {
    return Flags(value_ ^ static_cast<IntT>(flag));
  }

  constexpr Flags operator^(Flags flags) const {
    return Flags(value_ ^ flags.value_);
  }

  constexpr Flags operator~() const { return Flags(~value_); }

  explicit constexpr operator bool() const { return value_ != 0; }

  constexpr bool operator!() const { return value_ == 0; }

  constexpr bool operator==(Flags flags) const {
    return value_ == flags.value_;
  }

  constexpr bool operator!=(Flags flags) const {
    return value_ != flags.value_;
  }

  constexpr bool test(Enum flag) const {
    if (static_cast<IntT>(flag) == 0) {
      return value_ == 0;
    } else {
      return (value_ & static_cast<IntT>(flag)) == static_cast<IntT>(flag);
    }
  }

  constexpr Flags& set(Enum flag, bool on = true) {
    if (on) {
      value_ |= static_cast<IntT>(flag);
    } else {
      value_ &= ~static_cast<IntT>(flag);
    }
    return *this;
  }

  constexpr bool empty() const { return value_ == 0; }

  constexpr void clear() { value_ = 0; }

  constexpr bool is_subset_of(Flags flags) const {
    return (value_ | flags.value_) == flags.value_;
  }

  constexpr bool has_single_bit() const {
    return (value_ && !(value_ & (value_ - 1)));
  }

  constexpr IntT encode() const { return value_; }

  static constexpr Flags decode(IntT encoding) { return Flags(encoding); }

 private:
  explicit constexpr Flags(IntT value) : value_(value) {}

 private:
  IntT value_ = 0;
};

} // namespace sparta
