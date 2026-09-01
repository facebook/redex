/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"

struct PartialCode;

namespace inliner {

bool is_not_cold(cfg::Block* b);

bool maybe_hot(cfg::Block* b);

bool is_hot(cfg::Block* b);

/**
 * The opcode with which the partially inlined code invokes the method in its
 * fallback invocation. Whether that reproduces the invocation being replaced
 * is what `MultiMethodInliner::can_invoke_callee_directly` decides.
 */
IROpcode get_fallback_invoke_opcode(const DexMethod* method);

PartialCode get_partially_inlined_code(const DexMethod* method,
                                       const cfg::ControlFlowGraph& cfg,
                                       uint32_t max_code_units);

} // namespace inliner
