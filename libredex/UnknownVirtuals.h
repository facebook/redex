/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace unknown_virtuals {

bool is_method_known_to_be_public(DexMethodRef* method);

} // namespace unknown_virtuals
