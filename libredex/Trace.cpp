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
    const char* traceenv = getenv("TRACE");
    const char* envfile = getenv("TRACEFILE");
    if (!traceenv) {
      return;
    }
    init_trace_modules(traceenv);
    init_trace_file(envfile);
  }

  ~Tracer() {
    if (m_file) {
      fclose(m_file);
    }
  }

  bool traceEnabled(TraceModule module, int level) {
    return level <= m_level || level <= m_traces[module];
  }

  void trace(const char* fmt, va_list ap) {
    vfprintf(m_file, fmt, ap);
    fflush(m_file);
  }

 private:
  void init_trace_modules(const char* traceenv) {
    std::unordered_map<std::string, int> module_id_map;
#define TM(x) module_id_map[ #x ] = x;
    TMS
#undef TM
    char* tracespec = strdup(traceenv);
    const char* sep = ",: ";
    const char* tok = strtok(tracespec, sep);
    const char* module = nullptr;
    while (tok) {
      auto level = strtol(tok, nullptr, 10);
      if (level) {
        if (module) {
          if (module_id_map.count(module) == 0) {
            fprintf(stderr, "Unknown trace level %s\n", module);
            abort();
          }
          m_traces[module_id_map[module]] = level;
        } else {
          m_level = level;
        }
        module = nullptr;
      } else {
        module = tok;
      }
      tok = strtok(nullptr, sep);
    }
    free(tracespec);
  }

  void init_trace_file(const char* envfile) {
    if (!envfile) {
      envfile = "/dev/stderr";
    }
    m_file = fopen(envfile, "w");
    if (!m_file) {
      m_file = stderr;
    }
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
