/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "Util.h"

#define TMS          \
  TM(ADD_REDEX_TXT)  \
  TM(ACCESS)         \
  TM(API_UTILS)      \
  TM(ANNO)           \
  TM(ARGS)           \
  TM(BBPROFILE)      \
  TM(BIND)           \
  TM(BPH)            \
  TM(BRIDGE)         \
  TM(BUILDERS)       \
  TM(CLA)            \
  TM(CFG)            \
  TM(CFP)            \
  TM(CIC)            \
  TM(CLP_GQL)        \
  TM(CLP_LITHO)      \
  TM(CONSTP)         \
  TM(CPG)            \
  TM(CS)             \
  TM(CSE)            \
  TM(CU)             \
  TM(CUSTOMSORT)     \
  TM(DBGSTRIP)       \
  TM(DC)             \
  TM(DCE)            \
  TM(DEDUP_BLOCKS)   \
  TM(DEDUP_RES)      \
  TM(DELINIT)        \
  TM(DELMET)         \
  TM(DS)             \
  TM(EMPTY)          \
  TM(ENUM)           \
  TM(FINALINLINE)    \
  TM(HASHER)         \
  TM(HOTNESS)        \
  TM(ICONSTP)        \
  TM(IDEX)           \
  TM(INLINE)         \
  TM(GETTER)         \
  TM(INL)            \
  TM(INLRES)         \
  TM(INTRO_SWITCH)   \
  TM(INSTRUMENT)     \
  TM(INTF)           \
  TM(BLD_PATTERN)    \
  TM(LCR_PASS)       \
  TM(LIB)            \
  TM(LOC)            \
  TM(MAGIC_FIELDS)   \
  TM(MAIN)           \
  TM(MARIANA_TRENCH) \
  TM(METH_DEDUP)     \
  TM(METH_MERGER)    \
  TM(METH_PROF)      \
  TM(MMINL)          \
  TM(MMODE)          \
  TM(MONITOR)        \
  TM(MORTIROLO)      \
  TM(MTRANS)         \
  TM(OBFUSCATE)      \
  TM(OPTRES)         \
  TM(ORIGINALNAME)   \
  TM(OSDCE)          \
  TM(OUTLINE)        \
  TM(PEEPHOLE)       \
  TM(PGR)            \
  TM(PM)             \
  TM(PTA)            \
  TM(PURITY)         \
  TM(QUICK)          \
  TM(RAL)            \
  TM(REDEX)          \
  TM(REACH)          \
  TM(REACH_DUMP)     \
  TM(CHECKRECURSION) \
  TM(REFU)           \
  TM(REFL)           \
  TM(REG)            \
  TM(RELO)           \
  TM(RENAME)         \
  TM(RESO)           \
  TM(RG)             \
  TM(RME)            \
  TM(RMGOTO)         \
  TM(RMU)            \
  TM(RMUNINST)       \
  TM(RMUF)           \
  TM(RM_INTF)        \
  TM(RP)             \
  TM(SDIS)           \
  TM(SHORTEN)        \
  TM(SINK)           \
  TM(SPLIT_RES)      \
  TM(STATIC_RELO)    \
  TM(STRBUILD)       \
  TM(STR_CAT)        \
  TM(STR_SIMPLE)     \
  TM(SUPER)          \
  TM(SWITCH_EQUIV)   \
  TM(SYNT)           \
  TM(TIME)           \
  TM(TRACKRESOURCES) \
  TM(TYPE)           \
  TM(UCM)            \
  TM(UNTF)           \
  TM(VERIFY)         \
  TM(VMERGE)         \
  TM(VIRT)           \
  TM(TERA)           \
  TM(BRCR)           \
  TM(SWIN)           \
  TM(SWCL)           \
  TM(SW)             \
  TM(IFCS_ANALYSIS)  \
  TM(UNREF_INTF)     \
  TM(USES_NAMES)     \
  TM(OPT_STORES)     \
  TM(MEINT)          \
  TM(OPUT)           \
  TM(IODI)           \
  TM(MODULARITY)     \
  TM(VM)             \
  TM(POST_LOWERING)  \
  TM(LOOP)

enum TraceModule : int {
#define TM(x) x,
  TMS
#undef TM
      N_TRACE_MODULES,
};

// To avoid "-Wunused" warnings, keep the TRACE macros in common so that the
// compiler sees a "use." However, ensure that it is optimized away through
// a constexpr condition in NDEBUG mode.
#ifdef NDEBUG
constexpr bool traceEnabled(TraceModule, int) { return false; }
#else
bool traceEnabled(TraceModule module, int level);
#endif // NDEBUG

void trace(
    TraceModule module, int level, bool suppress_newline, const char* fmt, ...);
#define TRACE(module, level, fmt, ...)                                        \
  do {                                                                        \
    if (traceEnabled(module, level)) {                                        \
      trace(module, level, /* suppress_newline */ false, fmt, ##__VA_ARGS__); \
    }                                                                         \
  } while (0)
#define TRACE_NO_LINE(module, level, fmt, ...)                               \
  do {                                                                       \
    if (traceEnabled(module, level)) {                                       \
      trace(module, level, /* suppress_newline */ true, fmt, ##__VA_ARGS__); \
    }                                                                        \
  } while (0)

struct TraceContext {
  explicit TraceContext(const std::string& current_method) {
    s_current_method = &current_method;
  }
  ~TraceContext() { s_current_method = nullptr; }

  thread_local static const std::string* s_current_method;
  static std::mutex s_trace_mutex;
};
