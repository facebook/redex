/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "IRCode.h"
#include "IRList.h"

namespace editable_cfg_adapter {

/**
 * Given an IRCode object and a function to apply to each element, these
 * functions will iterate through whichever data structure actually holds the
 * code (because the editable CFG "steals" the code out of the IRCode). See the
 * comment at the top of ControlFlow.h for more details about the editable CFG.
 *
 * These methods are here to bridge the gap between the list backed IRCode
 * implementation and the editable graph based representation.
 */

enum LoopExit {
  LOOP_CONTINUE,
  LOOP_BREAK,
};

/**
 * Iterate through instructions only.
 * Function is of type MethodItemEntry& -> LoopExit
 */
template <typename Function>
void iterate(IRCode* code, Function func) {
  if (code->editable_cfg_built()) {
    for (MethodItemEntry& mie : cfg::InstructionIterable(code->cfg())) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    for (MethodItemEntry& mie : ir_list::InstructionIterable(code)) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

/**
 * Iterate through all types of `MethodItemEntry`s, not just instructions.
 * See IRList.h for a full description of the types of `MethodItemEntry`s
 *
 * Function is of type MethodItemEntry& -> LoopExit
 */
template <typename Function>
void iterate_all(IRCode* code, Function func) {
  if (code->editable_cfg_built()) {
    for (cfg::Block* b : code->cfg().blocks()) {
      for (auto& mie : *b) {
        if (func(mie) == LOOP_BREAK) {
          break;
        }
      }
    }
  } else {
    for (MethodItemEntry& mie : *code) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

/**
 * Iterate through instructions only
 * Function is IRList::Iterator -> LoopExit
 */
template <typename Function>
void iterate_with_iterator(IRCode* code, Function func) {
  if (code->editable_cfg_built()) {
    auto ii = cfg::InstructionIterable(code->cfg());
    const auto& end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      if (func(it.unwrap()) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    auto ii = ir_list::InstructionIterable(code);
    const auto& end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      if (func(it.unwrap()) == LOOP_BREAK) {
        break;
      }
    }
  }
}

/**
 * const versions of the above functions
 */
void iterate(const IRCode* code,
             std::function<LoopExit(const MethodItemEntry&)> func);

void iterate_all(const IRCode* code,
                 std::function<LoopExit(const MethodItemEntry&)> func);

void iterate_with_iterator(
    const IRCode* code, std::function<LoopExit(IRList::const_iterator)> func);

}; // namespace editable_cfg_adapter
