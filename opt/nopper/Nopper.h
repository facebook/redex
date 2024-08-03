/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

namespace nopper_impl {

std::vector<cfg::Block*> get_noppable_blocks(cfg::ControlFlowGraph& cfg);

size_t insert_nops(cfg::ControlFlowGraph& cfg,
                   const std::unordered_set<cfg::Block*>& blocks);

} // namespace nopper_impl
