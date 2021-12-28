/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace sparta {

namespace pt_util {

template <typename IntegerType>
inline IntegerType is_zero_bit(IntegerType k, IntegerType m) {
  return (k & m) == 0;
}

template <typename IntegerType>
inline IntegerType get_lowest_bit(IntegerType x) {
  return x & (~x + 1);
}

template <typename IntegerType>
inline IntegerType get_branching_bit(IntegerType prefix0, IntegerType prefix1) {
  return get_lowest_bit(prefix0 ^ prefix1);
}

template <typename IntegerType>
IntegerType mask(IntegerType k, IntegerType m) {
  return k & (m - 1);
}

template <typename IntegerType>
IntegerType match_prefix(IntegerType k, IntegerType p, IntegerType m) {
  return mask(k, m) == p;
}

template <typename T, typename = void>
struct Dereference {
  T operator()(T x) const { return x; }
};

template <typename T>
struct Dereference<T, std::enable_if_t<std::is_pointer<T>::value>> {
  const typename std::remove_pointer<T>::type& operator()(T x) const {
    return *x;
  }
};

} // namespace pt_util

} // namespace sparta
