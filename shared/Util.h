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

#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    auto cond_eval = (cond);                                                 \
    if (!cond_eval) {                                                        \
      fprintf(stderr, "%s:%d CHECK(%s) failed.", __FILE__, __LINE__, #cond); \
      fprintf(stderr, " " __VA_ARGS__);                                      \
      fprintf(stderr, "\n");                                                 \
    }                                                                        \
  } while (0)

#define UNCOPYABLE(klass)       \
  klass(const klass&) = delete; \
  klass& operator=(const klass&) = delete;

#define MOVABLE(klass)      \
  klass(klass&&) = default; \
  klass& operator=(klass&&) = default;

#ifdef __GNUC__
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

/**
 * Insert into the proper location in a sorted container.
 */
template <class Container, class T, class Compare>
void insert_sorted(Container& c, const T& e, Compare comp) {
  c.insert(std::lower_bound(c.begin(), c.end(), e, comp), e);
}

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

#ifdef __GNUC__
#define PACKED(class_to_pack) class_to_pack __attribute__((packed))
#elif _MSC_VER
#define PACKED(class_to_pack) \
  __pragma(pack(push, 1)) class_to_pack __pragma(pack(pop))
#else
#error "Please define PACKED"
#endif

#ifdef _MSC_VER
#include <BaseTsd.h>
using ssize_t = SSIZE_T;
#endif

#if defined(__clang__) || defined (__GNUC__)
# define NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
# define NO_SANITIZE_ADDRESS
#endif
