/**
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

enum LoopExit {
  LOOP_CONTINUE,
  LOOP_BREAK,
};

// Function is of type MethodItemEntry* -> LoopExit
template <typename Function>
void iterate(IRCode* code, Function func) {
  if (code->editable_cfg_built()) {
    for (MethodItemEntry& mie : cfg::InstructionIterable(code->cfg())) {
      if (func(&mie) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    for (MethodItemEntry& mie : ir_list::InstructionIterable(code)) {
      if (func(&mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

// Function is of type const MethodItemEntry& -> LoopExit
template <typename Function>
void iterate(const IRCode* code, Function func) {
  if (code->editable_cfg_built()) {
    for (const MethodItemEntry& mie :
         cfg::ConstInstructionIterable(code->cfg())) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    for (const MethodItemEntry& mie : ir_list::ConstInstructionIterable(code)) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

// Function is IRList::Iterator -> LoopExit
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

}; // namespace editable_cfg_adapter
