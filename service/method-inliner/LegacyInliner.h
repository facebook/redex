/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "IRCode.h"

/*
 * Functionality provided by the legacy inliner is not based on
 * control-flow-graphs, may not handle all subleties like source-blocks
 * properly, and is not aware of reasons or limits while inlining must not
 * happen. Use with care, and consider switching.
 */

namespace legacy_inliner {

/*
 * Inline tail-called `callee` into `caller` at `pos`.
 *
 * NB: This is NOT a general-purpose inliner; it assumes that the caller does
 * not do any work after the call, so the only live registers are the
 * parameters to the callee. This allows it to do inlining by simply renaming
 * the callee's registers. The more general inline_method instead inserts
 * move instructions to map the caller's argument registers to the callee's
 * params.
 *
 * In general, use of this method should be considered deprecated. It is
 * currently only being used by the BridgePass because the insertion of
 * additional move instructions would confuse SynthPass, which looks for
 * exact sequences of instructions.
 */
void inline_tail_call(DexMethod* caller,
                      DexMethod* callee,
                      IRList::iterator pos);

/*
 * Inline `callee` into `caller` at `pos` but not check if the caller method has
 * the permit to call the inlined code.
 *
 * `caller_method` is only used to synthesize a DexPosition entry, if necessary.
 * It is permissable to use nullptr, in which case no insertion takes place.
 */
void inline_method_unsafe(const DexMethod* caller_method,
                          IRCode* caller,
                          IRCode* callee,
                          const IRList::iterator& pos);

/**
 * Inline `callee` into `caller` at `pos` and try to change the visibility of
 * accessed members. See comment of `change_visibility` for details.
 */
void inline_method(DexMethod* caller,
                   IRCode* callee,
                   const IRList::iterator& pos);

} // namespace legacy_inliner
