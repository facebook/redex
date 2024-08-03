/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

namespace nopper_impl {

struct AuxiliaryDefs {
  DexClass* cls;
  DexField* int_field;
  DexMethod* fib_method;
  DexMethod* clinit;
};

AuxiliaryDefs create_auxiliary_defs(DexType* nopper_type);

std::vector<cfg::Block*> get_noppable_blocks(cfg::ControlFlowGraph& cfg);

size_t insert_nops(cfg::ControlFlowGraph& cfg,
                   const std::unordered_set<cfg::Block*>& blocks,
                   AuxiliaryDefs* auxiliary_defs = nullptr);

} // namespace nopper_impl
