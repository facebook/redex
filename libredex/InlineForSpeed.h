/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "MethodProfiles.h"

class InlineForSpeed {
 public:
  virtual ~InlineForSpeed() {}

  virtual bool should_inline(const DexMethod* caller_method,
                             const DexMethod* callee_method) = 0;
};
