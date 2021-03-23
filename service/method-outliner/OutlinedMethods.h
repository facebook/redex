/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace instruction_sequence_outliner {

constexpr const char* OUTLINED_METHOD_NAME_PREFIX = "$outlined$";

inline bool is_outlined_method(const DexMethodRef* method) {
  return strncmp(method->get_name()->c_str(), OUTLINED_METHOD_NAME_PREFIX,
                 strlen(OUTLINED_METHOD_NAME_PREFIX)) == 0;
}

} // namespace instruction_sequence_outliner
