/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

#include <sparta/PatriciaTreeKeyTrait.h>

namespace sparta {

namespace pt_util {

template <typename Key>
struct Codec {
  using IntegerType = typename PatriciaTreeKeyTrait<Key>::IntegerType;

  static_assert(std::is_unsigned_v<IntegerType>,
                "IntegerType is not an unsigned arithmetic type");

  static_assert(sizeof(IntegerType) == sizeof(Key),
                "IntegerType and Key must have the same size");

  static_assert(std::alignment_of<IntegerType>::value ==
                    std::alignment_of<Key>::value,
                "IntegerType and Key must have the same alignment");

  static const IntegerType& encode(const Key& key) {
    return reinterpret_cast<const IntegerType&>(key);
  }

  // To correctly conform to the forward iterator concept, these must
  // return reinterpreted references to storage - not copies.
  template <typename T>
  static std::enable_if_t<!std::is_lvalue_reference_v<T>> decode(T&&) = delete;

  static const Key& decode(const IntegerType& integer_key) {
    return reinterpret_cast<const Key&>(integer_key);
  }

  template <typename Value>
  static const std::pair<Key, Value>& decode(
      const std::pair<IntegerType, Value>& pair) {
    return reinterpret_cast<const std::pair<Key, Value>&>(pair);
  }
};

template <typename IntegerType>
inline bool is_zero_bit(IntegerType k, IntegerType m) {
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

template <typename T>
std::enable_if_t<!std::is_pointer_v<T>, const T&> deref(const T& x) {
  return x;
}

template <typename T>
const T& deref(const T* x) {
  return *x;
}

} // namespace pt_util

} // namespace sparta
