/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "Macros.h"
#include "Util.h"

class DexMethodRef;
class DexType;

#define TMS             \
  TM(ACCESS)            \
  TM(ANNO)              \
  TM(API_UTILS)         \
  TM(APP_MOD_USE)       \
  TM(ARGS)              \
  TM(ASSESSOR)          \
  TM(BBPROFILE)         \
  TM(BBREORDERING)      \
  TM(BIND)              \
  TM(BLD_PATTERN)       \
  TM(BPH)               \
  TM(BRCR)              \
  TM(BRIDGE)            \
  TM(BUILDERS)          \
  TM(CALLGRAPH)         \
  TM(CCB)               \
  TM(CDDP)              \
  TM(CFG)               \
  TM(CHECKRECURSION)    \
  TM(CIC)               \
  TM(CLA)               \
  TM(CLMG)              \
  TM(CLP_LITHO)         \
  TM(CONSTP)            \
  TM(CPG)               \
  TM(CS)                \
  TM(CSE)               \
  TM(CU)                \
  TM(CUSTOMSORT)        \
  TM(DBGSTRIP)          \
  TM(DC)                \
  TM(DCE)               \
  TM(DEDUP_BLOCKS)      \
  TM(DEDUP_RES)         \
  TM(DELINIT)           \
  TM(DELMET)            \
  TM(DS)                \
  TM(EMPTY)             \
  TM(ENUM)              \
  TM(EVALTC)            \
  TM(FINALINLINE)       \
  TM(FREG)              \
  TM(GETTER)            \
  TM(GQL)               \
  TM(HASHER)            \
  TM(ICL)               \
  TM(ICONSTP)           \
  TM(IDEX)              \
  TM(IDEXR)             \
  TM(IFCS_ANALYSIS)     \
  TM(III)               \
  TM(IIL)               \
  TM(INL)               \
  TM(INLINE)            \
  TM(INLRES)            \
  TM(INSTRUMENT)        \
  TM(INTF)              \
  TM(INTRO_SWITCH)      \
  TM(IODI)              \
  TM(ISO)               \
  TM(ITP)               \
  TM(LCR_PASS)          \
  TM(LIB)               \
  TM(LOC)               \
  TM(LOCKS)             \
  TM(LOOP)              \
  TM(MAGIC_FIELDS)      \
  TM(MAIN)              \
  TM(MARIANA_TRENCH)    \
  TM(MEINT)             \
  TM(METH_DEDUP)        \
  TM(METH_MERGER)       \
  TM(METH_PROF)         \
  TM(MFLOW)             \
  TM(MMINL)             \
  TM(MODULARITY)        \
  TM(MONITOR)           \
  TM(MORTIROLO)         \
  TM(MS)                \
  TM(MTRANS)            \
  TM(NATIVE)            \
  TM(NCI)               \
  TM(NULLCHECK)         \
  TM(OBFUSCATE)         \
  TM(OEA)               \
  TM(OBFUS_RES)         \
  TM(OPTRES)            \
  TM(OPT_STORES)        \
  TM(OPUT)              \
  TM(ORIGINALNAME)      \
  TM(OSDCE)             \
  TM(OUTLINE)           \
  TM(PA)                \
  TM(PEEPHOLE)          \
  TM(PGR)               \
  TM(PM)                \
  TM(POST_LOWERING)     \
  TM(PTA)               \
  TM(PURITY)            \
  TM(QUICK)             \
  TM(RABBIT)            \
  TM(RAL)               \
  TM(RBB)               \
  TM(REACH)             \
  TM(REFC)              \
  TM(REFL)              \
  TM(REFU)              \
  TM(REG)               \
  TM(RENAME)            \
  TM(RES)               \
  TM(RESO)              \
  TM(RG)                \
  TM(RME)               \
  TM(RMGOTO)            \
  TM(RM_INTF)           \
  TM(RMRCC)             \
  TM(RMU)               \
  TM(RMUF)              \
  TM(RMUNINST)          \
  TM(ROR)               \
  TM(RP)                \
  TM(SBCC)              \
  TM(SDIS)              \
  TM(SHORTEN)           \
  TM(SPLIT_RES)         \
  TM(SRC_PASS)          \
  TM(STATIC_RELO)       \
  TM(STATS)             \
  TM(STRBUILD)          \
  TM(STR_CAT)           \
  TM(SUPER)             \
  TM(SW)                \
  TM(SWIN)              \
  TM(SWITCH_EQUIV)      \
  TM(SYNT)              \
  TM(TIME)              \
  TM(TP)                \
  TM(TRACKRESOURCES)    \
  TM(TRMU)              \
  TM(TYPE)              \
  TM(TYPE_TRANSFORM)    \
  TM(UCM)               \
  TM(UNREF_INTF)        \
  TM(USES_NAMES)        \
  TM(VERIFY)            \
  TM(VIRT)              \
  TM(VM)                \
  TM(VMERGE)            \
  TM(KOTLIN_INSTANCE)   \
  TM(KOTLIN_STATS)      \
  TM(KOTLIN_OBJ_INLINE) \
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

void trace(TraceModule module,
           int level,
           bool suppress_newline,
           const char* fmt,
           ...) ATTR_FORMAT(4, 5);

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

class TraceContext {
 public:
  explicit TraceContext(const DexMethodRef* current_method) {
#if !IS_WINDOWS
    last_context = s_context;
    s_context = this;
    method = current_method;
    string_value = &string_value_cache;
#endif
  }
  explicit TraceContext(const DexType* current_type) {
#if !IS_WINDOWS
    last_context = s_context;
    s_context = this;
    type = current_type;
    string_value = &string_value_cache;
#endif
  }
  explicit TraceContext(const std::string* string_value) {
#if !IS_WINDOWS
    last_context = s_context;
    s_context = this;
    this->string_value = string_value;
#endif
  }
  ~TraceContext() {
#if !IS_WINDOWS
    s_context = last_context;
#endif
  }

#if !IS_WINDOWS
  const std::string& get_string_value() const;
  const DexMethodRef* get_dex_method_ref() const { return this->method; }
#endif

 private:
#if !IS_WINDOWS
  thread_local static const TraceContext* s_context;
  const TraceContext* last_context{nullptr};
  const DexMethodRef* method{nullptr};
  const DexType* type{nullptr};
  const std::string* string_value{nullptr};
  mutable std::string string_value_cache;
#endif

  friend struct TraceContextAccess;
};
