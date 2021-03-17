/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace std20 {

// Functionality that will be in C++20 STL.

template <typename Container, typename Pred>
void erase_if(Container& c, const Pred& pred) {
  for (auto it = c.begin(); it != c.end();) {
    if (pred(it)) {
      it = c.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace std20
