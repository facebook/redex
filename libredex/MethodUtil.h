/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace method {
/**
 * True if the method is a constructor (matches the "<init>" name)
 */
bool is_init(const DexMethodRef* method);

/**
 * True if the method is a static constructor (matches the "<clinit>" name)
 */
bool is_clinit(const DexMethodRef* method);

/**
 * Whether the method is a ctor or static ctor.
 */
inline bool is_any_init(const DexMethodRef* method) {
  return is_init(method) || is_clinit(method);
}

/**
 * Return true if the clinit is Trivial.
 * A trivial clinit should only contain a return-void instruction.
 */
bool is_trivial_clinit(const DexMethod* method);

}; // namespace method
