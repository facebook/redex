/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Trace.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

struct Tracer {
  Tracer() {
    std::unordered_map<std::string, int> module_id_map;
#define TM(x) module_id_map[ #x ] = x;
    TMS
#undef TM
    char* tracespec = getenv("TRACE");
    const char* envfile = getenv("TRACEFILE");
    if (tracespec) {
      m_level = strtol(tracespec, nullptr, 10);
      if (m_level == 0) {
        auto module = strtok(tracespec, ":");
        while (true) {
          if (!module) break;
          const char* strlevel = strtok(nullptr, " ,");
          if (!strlevel) break;
          auto level = strtol(strlevel, nullptr, 10);
          m_traces[module_id_map[module]] = level;
          module = strtok(nullptr, ":");
        }
      }
      if (!envfile) {
        envfile = "/dev/stderr";
      }
      m_file = fopen(envfile, "w");
      if (!m_file) {
        m_file = stderr;
      }
    }
  }

  ~Tracer() {
    if (m_file) {
      fclose(m_file);
    }
  }

  bool traceEnabled(TraceModule module, int level) {
    if (m_level == 0) {
      if (level > m_traces[module]) return false;
    } else if (level > m_level) {
      return false;
    }
    return true;
  }

  void trace(const char* fmt, va_list ap) {
    vfprintf(m_file, fmt, ap);
    fflush(m_file);
  }

 private:
  FILE* m_file{nullptr};
  int m_level{0};
  std::array<int, N_TRACE_MODULES> m_traces;
};

static Tracer tracer;
}

bool traceEnabled(TraceModule module, int level) {
  return tracer.traceEnabled(module, level);
}

void trace(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  tracer.trace(fmt, ap);
  va_end(ap);
}
