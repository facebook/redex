/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace sparta {

// Template helpers to get the array of a function type or similar.
template <typename T, bool IS_FUNC>
struct GetArity;

template <typename T>
struct GetArity<T, true> {
  template <typename R>
  struct get_arity;
  template <typename R, typename... Args>
  struct get_arity<R(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename... Args>
  struct get_arity<R (*)(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename C, typename... Args>
  struct get_arity<R (C::*)(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename C, typename... Args>
  struct get_arity<R (C::*)(Args...) const>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  static constexpr unsigned value = get_arity<T>::value;
};

template <typename T>
struct GetArity<T, false> {
  template <typename R>
  struct get_arity : get_arity<decltype(&R::operator())> {};
  template <typename R, typename... Args>
  struct get_arity<R(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename... Args>
  struct get_arity<R (*)(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename C, typename... Args>
  struct get_arity<R (C::*)(Args...)>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  template <typename R, typename C, typename... Args>
  struct get_arity<R (C::*)(Args...) const>
      : std::integral_constant<unsigned, sizeof...(Args)> {};
  static constexpr unsigned value = get_arity<T>::value;
};

template <typename T>
struct Arity : GetArity<T, std::is_function<T>::value> {};
} // namespace sparta
