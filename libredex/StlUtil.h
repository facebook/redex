/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
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
constexpr To bit_cast(const From& src) noexcept {
  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}

} // namespace std20

namespace std23 {
// Functionality that will be in C++23 STL.
template <class Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(e);
}

} // namespace std23
