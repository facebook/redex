/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "MethodProfiles.h"

namespace cfg {
class Block;
} // namespace cfg

class InlineForSpeed {
 public:
  virtual ~InlineForSpeed() {}

  // Whether to inline the given callee method into the given caller in the
  // context of the given callsite (independent of callsite).
  virtual bool should_inline_generic(const DexMethod* caller_method,
                                     const DexMethod* callee_method) = 0;

  // Whether to inline the given callee method into the given caller in the
  // context of the given callsite (dependent of callsite).
  virtual bool should_inline_callsite(const DexMethod* caller_method,
                                      const DexMethod* callee_method,
                                      const cfg::Block* caller_block) = 0;
};
