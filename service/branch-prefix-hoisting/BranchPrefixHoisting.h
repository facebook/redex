/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantUses.h"
#include "Lazy.h"

class IRCode;

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace branch_prefix_hoisting_impl {

size_t process_code(IRCode*, DexMethod*, bool can_allocate_regs = true);
size_t process_cfg(cfg::ControlFlowGraph&,
                   Lazy<const constant_uses::ConstantUses>&,
                   bool can_allocate_regs = true);
} // namespace branch_prefix_hoisting_impl
