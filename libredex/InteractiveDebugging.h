/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Macros.h"
#include <stddef.h>

#if !defined(NDEBUG) && !IS_WINDOWS

namespace cfg {
class ControlFlowGraph;
class Block;
using BlockId = size_t;
} // namespace cfg
// namespace cfg
class IRCode;

// The following APIs are intended to be called from the debugger (athough
// there's no harm calling them from internal code if desired).
// __attribute__((used)) is important so that the linker does not discard them,
// as they may not be referenced by any other code.  Consider making LLDB
// aliases in redex/.lldbinit for functions in this file to make them easier to
// call when debugging.

const char* methname()
    __attribute__((used)); // NOTE: This allocates the string which is returned.
void dumpmethname() __attribute__((used));
void dumpir() __attribute__((used));
void dumpir(const IRCode* ir_code) __attribute__((used));
void dumpcfg() __attribute__((used));
void dumpcfg(const cfg::ControlFlowGraph& cfg) __attribute__((used));
void dumpblock(const cfg::Block* block) __attribute__((used));
void dumpblock(cfg::BlockId block_id) __attribute__((used));
void setdumpfile(const char* file_name) __attribute__((used));
void setdumpfilemode(const char* mode) __attribute__((used));

// Helper macros
// Note: These work with Clang, and likely need modification to work on other
// compilers.

// Break into the debugger when the current method's name contains the passed in
// substring
#define METHBREAK(meth_name_substr)                         \
  if (methname() && strstr(methname(), meth_name_substr)) { \
    __builtin_debugtrap();                                  \
  }

// Break into the debugger when the current method's name exactly matches the
// passed in name
#define METHBREAK_EXACT(meth_name)                        \
  if (methname() && strcmp(methname(), meth_name) == 0) { \
    __builtin_debugtrap();                                \
  }

// Convenience wrappers for disabling optimizations for a function or set of
// functions. This allows for easier debugging of specific files/functions,
// without making the rest of Redex slow.
// Typical usage would be to place OPT_OFF just below includes at the top of a
// CPP file, or to wrap a function in an OPT_OFF/OPT_ON pair.
#define OPT_OFF _Pragma("clang optimize off")
#define OPT_ON _Pragma("clang optimize on")

#endif
