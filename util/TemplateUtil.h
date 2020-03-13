/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace template_util {

// Extract the first type in a parameter pack.
template <typename Head, typename... Tail>
struct HeadType {
  using type = Head;
};

// Check if all template parameters are true.
// See
// https://stackoverflow.com/questions/28253399/check-traits-for-all-variadic-template-arguments/28253503#28253503
template <bool...>
struct bool_pack;
template <bool... v>
using all_true = std::is_same<bool_pack<true, v...>, bool_pack<v..., true>>;

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
