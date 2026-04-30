/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>

#include "ControlFlow.h"
#include "IRCode.h"
#include "IRList.h"

namespace cfg_adapter {

/**
 * Given an IRCode object and a function to apply to each element, these
 * functions will iterate through whichever data structure actually holds the
 * code (because the CFG "steals" the code out of the IRCode). See the
 * comment at the top of ControlFlow.h for more details about the CFG.
 *
 * These methods are here to bridge the gap between the list backed IRCode
 * implementation and the graph based representation.
 */

enum LoopExit {
  LOOP_CONTINUE,
  LOOP_BREAK,
};

namespace impl {

template <typename IRCodeType>
using cfgInstructionIterable = std::conditional_t<std::is_const_v<IRCodeType>,
                                                  cfg::ConstInstructionIterable,
                                                  cfg::InstructionIterable>;

template <typename IRCodeType>
using irlistInstructionIterable =
    std::conditional_t<std::is_const_v<IRCodeType>,
                       ir_list::ConstInstructionIterable,
                       ir_list::InstructionIterable>;

template <typename IRCodeType, typename Function>
void iterate(IRCodeType* code, const Function& func) {
  if (code->cfg_built()) {
    for (auto& mie : cfgInstructionIterable<IRCodeType>(code->cfg())) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    for (auto& mie : irlistInstructionIterable<IRCodeType>(code)) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

template <typename IRCodeType, typename Function>
void iterate_all(IRCodeType* code, const Function& func) {
  if (code->cfg_built()) {
    for (cfg::Block* b : code->cfg().blocks()) {
      for (auto& mie : *b) {
        if (func(mie) == LOOP_BREAK) {
          break;
        }
      }
    }
  } else {
    for (auto& mie : *code) {
      if (func(mie) == LOOP_BREAK) {
        break;
      }
    }
  }
}

template <typename IRCodeType, typename Function>
void iterate_with_iterator(IRCodeType* code, const Function& func) {
  if (code->cfg_built()) {
    auto ii = cfgInstructionIterable<IRCodeType>(code->cfg());
    const auto& end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      if (func(it.unwrap()) == LOOP_BREAK) {
        break;
      }
    }
  } else {
    auto ii = irlistInstructionIterable<IRCodeType>(code);
    const auto& end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      if (func(it.unwrap()) == LOOP_BREAK) {
        break;
      }
    }
  }
}
} // namespace impl

/**
 * Iterate through instructions only.
 */
inline void iterate(IRCode* code,
                    const std::function<LoopExit(MethodItemEntry&)>& func) {
  impl::iterate(code, func);
}
inline void iterate(
    const IRCode* code,
    const std::function<LoopExit(const MethodItemEntry&)>& func) {
  impl::iterate(code, func);
}

/**
 * Iterate through all types of `MethodItemEntry`s, not just instructions.
 * See IRList.h for a full description of the types of `MethodItemEntry`s.
 */
inline void iterate_all(IRCode* code,
                        const std::function<LoopExit(MethodItemEntry&)>& func) {
  impl::iterate_all(code, func);
}
inline void iterate_all(
    const IRCode* code,
    const std::function<LoopExit(const MethodItemEntry&)>& func) {
  impl::iterate_all(code, func);
}

/**
 * Iterate through instructions only
 */
inline void iterate_with_iterator(
    IRCode* code, const std::function<LoopExit(IRList::iterator)>& func) {
  impl::iterate_with_iterator(code, func);
}
inline void iterate_with_iterator(
    const IRCode* code,
    const std::function<LoopExit(IRList::const_iterator)>& func) {
  impl::iterate_with_iterator(code, func);
}
}; // namespace cfg_adapter
