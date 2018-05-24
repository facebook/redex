/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

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

} // namespace pt_util
