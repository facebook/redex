/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexClass.h"

struct PartialCode;

namespace inliner {

bool is_not_cold(cfg::Block* b);

bool maybe_hot(cfg::Block* b);

bool is_hot(cfg::Block* b);

PartialCode get_partially_inlined_code(const DexMethod* method,
                                       const cfg::ControlFlowGraph& cfg);

} // namespace inliner
