/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Macros.h"

#if !defined(NDEBUG) && !IS_WINDOWS

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"

// The following APIs are intended to be called from the debugger (athough
// there's no harm calling them from internal code if desired).
// __attribute__((used)) is important so that the linker does not discard them,
// as they may not be referenced by any other code.  Consider making LLDB
// aliases in redex/.lldbinit for functions in this file to make them easier to
// call when debugging.

void dumpir() __attribute__((used));
void dumpir(const IRCode* ir_code) __attribute__((used));
void dumpcfg() __attribute__((used));
void dumpcfg(const cfg::ControlFlowGraph& cfg) __attribute__((used));
void dumpblock(const cfg::Block* block) __attribute__((used));
void dumpblock(cfg::BlockId block_id) __attribute__((used));
void setdumpfile(const char* file_name) __attribute__((used));
void setdumpfilemode(const char* mode) __attribute__((used));
#endif
