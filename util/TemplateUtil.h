/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

namespace template_util {

// Extract the first type in a parameter pack.
template <typename Head, typename... Tail>
struct HeadType {
  using type = Head;
};

// Check if all template parameters are true.
// See https://stackoverflow.com/questions/28253399/check-traits-for-all-variadic-template-arguments/28253503#28253503
template <bool...> struct bool_pack;
template <bool... v>
using all_true = std::is_same<bool_pack<true, v...>, bool_pack<v..., true>>;

} // namespace template_util
