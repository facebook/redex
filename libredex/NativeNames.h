/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "DexClass.h"

namespace native_names {
// If native method is not overloaded with another native method that has the
// same name, use short name that doesn't contain the parameter types.
std::string get_native_short_name_for_method(DexMethodRef* method);

// We need to use the long name only when a native method is overloaded with
// another native method.
std::string get_native_long_name_for_method(DexMethodRef* method);
} // namespace native_names
