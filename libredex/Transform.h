/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRCode.h"

#include <unordered_set>

#include "ControlFlow.h"
#include "IRInstruction.h"

namespace transform {

using RegMap = std::unordered_map<uint16_t, uint16_t>;

void remap_registers(IRCode*, const RegMap&);
void remap_registers(IRInstruction* insn, const RegMap& reg_map);
void remap_registers(MethodItemEntry& mei, const RegMap& reg_map);

void visit(cfg::Block* b, std::unordered_set<cfg::Block*>& visited);

/*
 * Sets all the opcodes in unreachable blocks to MFLOW_FALLTHROUGH, and removes
 * all successor edges connecting them to the graph. Does not actually delete
 * the blocks themselves.
 *
 * Return the number of instructions removed.
 */
size_t remove_unreachable_blocks(IRCode* code);

// TODO: move to CFG
// remove old_block
// if new_block is not null, reroute old_targets predecessors to new_target
void replace_block(IRCode* code, cfg::Block* old_block, cfg::Block* new_block);

// if pos is inside a try block, return the corresponding catch
// if not, return null
MethodItemEntry* find_active_catch(IRCode* code, IRList::iterator pos);

} // namespace transform
