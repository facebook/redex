/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace template_util {

template <typename...>
struct contains;

template <typename T>
struct contains<T> : std::false_type {};

template <typename T, typename Head, typename... Ts>
struct contains<T, Head, Ts...> {
  static constexpr bool value =
      std::is_same<T, Head>::value || contains<T, Ts...>::value;
};

} // namespace template_util
