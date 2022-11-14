/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRCode.h"

#include <unordered_set>

#include "ControlFlow.h"
#include "IRInstruction.h"

namespace transform {

using RegMap = std::unordered_map<reg_t, reg_t>;

void remap_registers(IRCode*, const RegMap&);
void remap_registers(cfg::ControlFlowGraph&, const RegMap&);
void remap_registers(IRInstruction* insn, const RegMap& reg_map);
void remap_registers(MethodItemEntry& mei, const RegMap& reg_map);

// if pos is inside a try block, return the corresponding catch
// if not, return null
MethodItemEntry* find_active_catch(IRCode* code, IRList::iterator pos);

} // namespace transform
