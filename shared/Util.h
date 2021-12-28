/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <memory>
#include <type_traits>
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

template <class T>
struct MergeContainers {
  void operator()(const T& addend, T* accumulator) {
    accumulator->insert(addend.begin(), addend.end());
  }
};

/**
 * Copy the const-ness of `In` onto `Out`.
 * For example:
 *   mimic_const_t<const Foo, Bar> == const Bar
 *   mimic_const_t<Foo, Bar>       == Bar
 */
template <typename In, typename Out>
struct mimic_const {
  using type =
      typename std::conditional_t<std::is_const<In>::value, const Out, Out>;
};
template <typename In, typename Out>
using mimic_const_t = typename mimic_const<In, Out>::type;

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

#if defined(__clang__) || defined(__GNUC__)
#define NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
#define NO_SANITIZE_ADDRESS
#endif

struct EnumClassHash {
  template <typename T>
  size_t operator()(T t) const {
    return static_cast<size_t>(t);
  }
};
