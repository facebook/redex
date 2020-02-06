/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EditableCfgAdapter.h"

#include <utility>

namespace editable_cfg_adapter {

void iterate(const IRCode* code,
             std::function<LoopExit(const MethodItemEntry&)> func) {
  // It's safe to use a const_cast here because `func` can't modify its
  // arguments and `iterate` doesn't make changes itself.
  iterate(const_cast<IRCode*>(code), std::move(func));
}

void iterate_all(const IRCode* code,
                 std::function<LoopExit(const MethodItemEntry&)> func) {
  iterate_all(const_cast<IRCode*>(code), std::move(func));
}

void iterate_with_iterator(
    const IRCode* code, std::function<LoopExit(IRList::const_iterator)> func) {
  iterate_with_iterator(const_cast<IRCode*>(code), std::move(func));
}
} // namespace editable_cfg_adapter
