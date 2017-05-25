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
#include <memory>
#include <utility>

namespace std {

#if __cplusplus<201300L
// simple implementation of make_unique since C++11 doesn't have it available
// note that it doesn't work properly if T is an array type
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
#endif
}


/**
 * Insert into the proper location in a sorted container.
 */
template <class Container, class T, class Compare>
void insert_sorted(Container& c, const T& e, Compare comp) {
  c.insert(std::lower_bound(c.begin(), c.end(), e, comp), e);
}

template<typename T> struct fake_dependency: public std::false_type {};

#define DISALLOW_DEFAULT_COMPARATOR(klass) \
  namespace std { \
  template <typename T, typename A> \
  class map<const klass*, T, std::less<const klass*>, A> { \
    static_assert(fake_dependency<T>::value, #klass " must not use default pointer comparison in std::map"); \
  }; \
  template <typename T, typename A> \
  class multimap<const klass*, T, std::less<const klass*>, A> { \
    static_assert(fake_dependency<T>::value, #klass " must not use default pointer comparison in std::multi_map"); \
  }; \
  template <typename A> \
  class set<const klass*, std::less<const klass*>, A> { \
    static_assert(fake_dependency<A>::value, #klass " must not use default pointer comparison in std::set"); \
  }; \
  template <typename A> \
  class multiset<const klass*, std::less<const klass*>, A> { \
    static_assert(fake_dependency<A>::value, #klass " must not use default pointer comparison in std::set"); \
  }; \
  }
