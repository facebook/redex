/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace sparta {

// To achieve a high level of metaprogramming, we use a little C++ trick to
// check for existence of a member function. This is implemented with SFINAE.
//
// More details:
// https://en.wikibooks.org/wiki/More_C%2B%2B_Idioms/Member_Detector
// https://stackoverflow.com/questions/257288/templated-check-for-the-existence-of-a-class-member-function

#define SPARTA_HAS_MEMBER_FUNCTION_WITH_SIGNATURE(func, name)   \
  template <typename T, typename Sign>                          \
  struct name {                                                 \
    typedef char yes[1];                                        \
    typedef char no[2];                                         \
    template <typename U, U>                                    \
    struct type_check;                                          \
    template <typename _1>                                      \
    static yes& chk(type_check<Sign, &_1::func>*);              \
    template <typename>                                         \
    static no& chk(...);                                        \
    static bool const value = sizeof(chk<T>(0)) == sizeof(yes); \
  }

#define SPARTA_HAS_STATIC_MEMBER_FUNCTION(func, name) \
  template <class, class = void>                      \
  struct name : std::false_type {};                   \
                                                      \
  template <class T>                                  \
  struct name<T, std::void_t<decltype(&T::func)>> : std::true_type {};

} // namespace sparta
