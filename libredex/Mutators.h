/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"

namespace mutators {

enum class KeepThis {
  No,
  Yes,
};

// Make a non-static direct or virtual method into a static method.
void make_static(DexMethod* method, KeepThis = KeepThis::Yes);

// Makes a static method into a non-static direct or virtual method.
// Limitation: First parameter must be of class type.
void make_non_static(DexMethod* method, bool make_virtual);
}
