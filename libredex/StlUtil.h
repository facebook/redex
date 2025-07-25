/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <type_traits>
#include <vector>

namespace std20 {

// Functionality that will be in C++20 STL.

template <typename Element, typename Pred>
size_t erase_if(std::vector<Element>& c, const Pred& pred) {
  auto it = std::remove_if(c.begin(), c.end(), pred);
  auto removed = std::distance(it, c.end());
  c.erase(it, c.end());
  return removed;
}

template <typename Element, typename Pred>
size_t erase_if(std::deque<Element>& c, const Pred& pred) {
  auto it = std::remove_if(c.begin(), c.end(), pred);
  auto removed = std::distance(it, c.end());
  c.erase(it, c.end());
  return removed;
}

template <typename Container, typename Pred>
size_t erase_if(Container& c, const Pred& pred) {
  size_t removed = 0;
  for (auto it = c.begin(), end = c.end(); it != end;) {
    if (pred(*it)) {
      it = c.erase(it);
      removed++;
    } else {
      ++it;
    }
  }
  return removed;
}

template <class To, class From>
constexpr std::enable_if_t<sizeof(To) == sizeof(From) &&
                               std::is_trivially_copyable_v<From> &&
                               std::is_trivially_copyable_v<To>,
                           To>
bit_cast(const From& src) noexcept {
  static_assert(std::is_trivially_constructible_v<To>,
                "This implementation additionally requires destination type to "
                "be trivially constructible");
  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}

template <class T>
constexpr int popcount(T x) noexcept {
  static_assert(std::is_unsigned<T>::value,
                "popcount requires an unsigned integer type");
#if defined(_MSC_VER) && defined(_M_X64)
  // MSVC 64-bit compiler intrinsic
  if constexpr (sizeof(T) <= sizeof(unsigned int)) {
    return __popcnt(static_cast<unsigned int>(x));
  } else {
    return __popcnt64(static_cast<unsigned __int64>(x));
  }
#elif defined(__GNUC__) || defined(__clang__)
  if constexpr (sizeof(T) <= sizeof(unsigned int)) {
    return __builtin_popcount(static_cast<unsigned int>(x));
  } else if constexpr (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_popcountl(static_cast<unsigned long>(x));
  } else {
    return __builtin_popcountll(static_cast<unsigned long long>(x));
  }
#else
  // Portable fallback
  int count = 0;
  while (x) {
    count += x & 1;
    x >>= 1;
  }
  return count;
#endif
}

template <class T>
constexpr int countr_zero(T x) noexcept {
  static_assert(std::is_unsigned<T>::value,
                "countr_zero requires an unsigned integer type");
  if (x == 0) return sizeof(T) * 8;
#if defined(_MSC_VER)
#include <intrin.h>
  if constexpr (sizeof(T) <= sizeof(unsigned long)) {
    unsigned long index;
    if (_BitScanForward(&index, static_cast<unsigned long>(x)))
      return static_cast<int>(index);
    else
      return sizeof(T) * 8;
  } else if constexpr (sizeof(T) <= sizeof(unsigned __int64)) {
    unsigned long index;
    if (_BitScanForward64(&index, static_cast<unsigned __int64>(x)))
      return static_cast<int>(index);
    else
      return sizeof(T) * 8;
  } else {
    // Portable fallback for larger types
    int count = 0;
    T mask = T(1);
    while ((x & mask) == 0) {
      ++count;
      mask <<= 1;
    }
    return count;
  }
#elif defined(__GNUC__) || defined(__clang__)
  if constexpr (sizeof(T) <= sizeof(unsigned int)) {
    return __builtin_ctz(static_cast<unsigned int>(x));
  } else if constexpr (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_ctzl(static_cast<unsigned long>(x));
  } else {
    return __builtin_ctzll(static_cast<unsigned long long>(x));
  }
#else
  // Portable fallback
  int count = 0;
  T mask = T(1);
  while ((x & mask) == 0) {
    ++count;
    mask <<= 1;
  }
  return count;
#endif
}
} // namespace std20

namespace std23 {
// Functionality that will be in C++23 STL.
template <class Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(e);
}

} // namespace std23
