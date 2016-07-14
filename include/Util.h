/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <algorithm>

/**
 * Insert into the proper location in a sorted container.
 */
template <class Container, class T, class Compare>
void insert_sorted(Container& c, const T& e, Compare comp) {
  c.insert(std::lower_bound(c.begin(), c.end(), e, comp), e);
}
