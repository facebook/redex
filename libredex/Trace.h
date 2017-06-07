/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <memory>
#include <string>

#include "Util.h"

#pragma once

#define TMS              \
  TM(ADD_REDEX_TXT)      \
  TM(ACCESS)             \
  TM(ANNO)               \
  TM(BIND)               \
  TM(BRIDGE)             \
  TM(BUILDERS)           \
  TM(COMP_BUILDERS)      \
  TM(CFG)                \
  TM(CFP)                \
  TM(CPG)                \
  TM(CONSTP)             \
  TM(CUSTOMSORT)         \
  TM(DBGSTRIP)           \
  TM(DC)                 \
  TM(DCE)                \
  TM(DELINIT)            \
  TM(DELMET)             \
  TM(DRAC)               \
  TM(EMPTY)              \
  TM(FINALINLINE)        \
  TM(IDEX)               \
  TM(INL)                \
  TM(INTF)               \
  TM(LOC)                \
  TM(MAGIC_FIELDS)       \
  TM(MAIN)               \
  TM(MMINL)              \
  TM(MORTIROLO)          \
  TM(MTRANS)             \
  TM(OBFUSCATE)          \
  TM(ORIGINALNAME)       \
  TM(OUTLINE)            \
  TM(PEEPHOLE)           \
  TM(PGR)                \
  TM(PM)                 \
  TM(REG)                \
  TM(RELO)               \
  TM(RENAME)             \
  TM(RME)                \
  TM(RMGOTO)             \
  TM(RMU)                \
  TM(SHORTEN)            \
  TM(SINK)               \
  TM(SINL)               \
  TM(SUPER)              \
  TM(SYNT)               \
  TM(TIME)               \
  TM(TRACKRESOURCES)     \
  TM(UNTF)               \
  TM(VERIFY)             \
  TM(ANALYSIS_REF_GRAPH) \
  TM(VIRT)               \
  TM(TERA)               \
  TM(BRCR)               \
  TM(SWIN)               \
  TM(SWCL)               \
  TM(SW)

enum TraceModule : int {
#define TM(x) x,
TMS
#undef TM
  N_TRACE_MODULES,
};

#ifdef NDEBUG
#define TRACE(...)
#else
bool traceEnabled(TraceModule module, int level);
void trace(TraceModule module, int level, const char* fmt, ...);
#define TRACE(module, level, fmt, ...)          \
  do {                                          \
    if (traceEnabled(module, level)) {          \
      trace(module, level, fmt, ##__VA_ARGS__); \
    }                                           \
  } while (0)
#endif // NDEBUG

struct TraceContext {
  explicit TraceContext(const std::string& current_method) {
    s_current_method = std::make_unique<std::string>(current_method);
  }
  ~TraceContext() {
    s_current_method = nullptr;
  }

  static std::unique_ptr<std::string> s_current_method;
};
