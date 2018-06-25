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

void make_static(DexMethod* method, KeepThis = KeepThis::Yes);

}
