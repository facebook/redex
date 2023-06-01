/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/algorithm/string/predicate.hpp>

#include "DexClass.h"

namespace outliner {

constexpr const char* OUTLINED_METHOD_NAME_PREFIX = "$outlined$";
constexpr const char* OUTLINED_METHOD_SHORT_NAME_PREFIX = "$o$";

inline bool is_outlined_method(const DexMethodRef* method) {
  const auto name = method->get_name()->str();
  const auto deobfuscated_name =
      method->as_def() ? method->as_def()->get_simple_deobfuscated_name() : "";
  return boost::algorithm::starts_with(name, OUTLINED_METHOD_NAME_PREFIX) ||
         boost::algorithm::starts_with(name,
                                       OUTLINED_METHOD_SHORT_NAME_PREFIX) ||
         boost::algorithm::starts_with(deobfuscated_name,
                                       OUTLINED_METHOD_NAME_PREFIX) ||
         boost::algorithm::starts_with(deobfuscated_name,
                                       OUTLINED_METHOD_SHORT_NAME_PREFIX);
}

} // namespace outliner
