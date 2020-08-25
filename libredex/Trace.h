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
  TM(ACCESS)         \
  TM(ANNO)           \
  TM(API_UTILS)      \
  TM(ARGS)           \
  TM(BBPROFILE)      \
  TM(BIND)           \
  TM(BLD_PATTERN)    \
  TM(BPH)            \
  TM(BRCR)           \
  TM(BRIDGE)         \
  TM(BUILDERS)       \
  TM(CFG)            \
  TM(CFP)            \
  TM(CHECKRECURSION) \
  TM(CIC)            \
  TM(CLA)            \
  TM(CLMG)           \
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
  TM(EVALTC)         \
  TM(FINALINLINE)    \
  TM(GETTER)         \
  TM(HASHER)         \
  TM(ICONSTP)        \
  TM(IDEX)           \
  TM(IFCS_ANALYSIS)  \
  TM(INL)            \
  TM(INLINE)         \
  TM(INLRES)         \
  TM(INSTRUMENT)     \
  TM(INTF)           \
  TM(INTRO_SWITCH)   \
  TM(IODI)           \
  TM(ISO)            \
  TM(LCR_PASS)       \
  TM(LIB)            \
  TM(LOC)            \
  TM(LOCKS)          \
  TM(LOOP)           \
  TM(MAGIC_FIELDS)   \
  TM(MAIN)           \
  TM(MARIANA_TRENCH) \
  TM(MEINT)          \
  TM(METH_DEDUP)     \
  TM(METH_MERGER)    \
  TM(METH_PROF)      \
  TM(MMINL)          \
  TM(MODULARITY)     \
  TM(MONITOR)        \
  TM(MORTIROLO)      \
  TM(MTRANS)         \
  TM(NULLCHECK)      \
  TM(OBFUSCATE)      \
  TM(OPTRES)         \
  TM(OPT_STORES)     \
  TM(OPUT)           \
  TM(ORIGINALNAME)   \
  TM(OSDCE)          \
  TM(OUTLINE)        \
  TM(PEEPHOLE)       \
  TM(PGR)            \
  TM(PM)             \
  TM(POST_LOWERING)  \
  TM(PTA)            \
  TM(PURITY)         \
  TM(QUICK)          \
  TM(RAL)            \
  TM(REACH)          \
  TM(REFL)           \
  TM(REFU)           \
  TM(REG)            \
  TM(RENAME)         \
  TM(RES)            \
  TM(RESO)           \
  TM(RG)             \
  TM(RME)            \
  TM(RMGOTO)         \
  TM(RM_INTF)        \
  TM(RMU)            \
  TM(RMUF)           \
  TM(RMUNINST)       \
  TM(RP)             \
  TM(SDIS)           \
  TM(SHORTEN)        \
  TM(SPLIT_RES)      \
  TM(STATIC_RELO)    \
  TM(STATS)          \
  TM(STRBUILD)       \
  TM(STR_CAT)        \
  TM(SUPER)          \
  TM(SW)             \
  TM(SWIN)           \
  TM(SWITCH_EQUIV)   \
  TM(SYNT)           \
  TM(TIME)           \
  TM(TP)             \
  TM(TRACKRESOURCES) \
  TM(TYPE)           \
  TM(TYPE_TRANSFORM) \
  TM(UCM)            \
  TM(UNREF_INTF)     \
  TM(USES_NAMES)     \
  TM(VERIFY)         \
  TM(VIRT)           \
  TM(VM)             \
  TM(VMERGE)         \
  /* End of list */

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
