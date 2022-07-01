/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>
#include <utility>

template <typename T>
struct fake_dependency : public std::false_type {};

#define DISALLOW_DEFAULT_COMPARATOR(klass)                                   \
  namespace std {                                                            \
  template <typename T, typename A>                                          \
  class map<const klass*, T, std::less<const klass*>, A> {                   \
    static_assert(fake_dependency<T>::value,                                 \
                  #klass                                                     \
                  " must not use default pointer comparison in std::map");   \
  };                                                                         \
  template <typename T, typename A>                                          \
  class multimap<const klass*, T, std::less<const klass*>, A> {              \
    static_assert(                                                           \
        fake_dependency<T>::value,                                           \
        #klass " must not use default pointer comparison in std::multimap"); \
  };                                                                         \
  template <typename A>                                                      \
  class set<const klass*, std::less<const klass*>, A> {                      \
    static_assert(fake_dependency<A>::value,                                 \
                  #klass                                                     \
                  " must not use default pointer comparison in std::set");   \
  };                                                                         \
  template <typename A>                                                      \
  class multiset<const klass*, std::less<const klass*>, A> {                 \
    static_assert(                                                           \
        fake_dependency<A>::value,                                           \
        #klass " must not use default pointer comparison in std::multiset"); \
  };                                                                         \
                                                                             \
  template <typename T, typename A>                                          \
  class map<klass*, T, std::less<klass*>, A> {                               \
    static_assert(fake_dependency<T>::value,                                 \
                  #klass                                                     \
                  " must not use default pointer comparison in std::map");   \
  };                                                                         \
  template <typename T, typename A>                                          \
  class multimap<klass*, T, std::less<klass*>, A> {                          \
    static_assert(                                                           \
        fake_dependency<T>::value,                                           \
        #klass " must not use default pointer comparison in std::multimap"); \
  };                                                                         \
  template <typename A>                                                      \
  class set<klass*, std::less<klass*>, A> {                                  \
    static_assert(fake_dependency<A>::value,                                 \
                  #klass                                                     \
                  " must not use default pointer comparison in std::set");   \
  };                                                                         \
  template <typename A>                                                      \
  class multiset<klass*, std::less<klass*>, A> {                             \
    static_assert(                                                           \
        fake_dependency<A>::value,                                           \
        #klass " must not use default pointer comparison in std::multiset"); \
  };                                                                         \
  }
