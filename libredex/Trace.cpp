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

  bool m_show_timestamps{false};
  const char* m_method_filter;

  Tracer() {
    const char* traceenv = getenv("TRACE");
    const char* envfile = getenv("TRACEFILE");
    const char* show_timestamps = getenv("SHOW_TIMESTAMPS");
    m_method_filter = getenv("TRACE_METHOD_FILTER");
    if (!traceenv) {
      return;
    }
    init_trace_modules(traceenv);
    init_trace_file(envfile);

    if (show_timestamps) {
      m_show_timestamps = true;
    }
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
    if (m_method_filter && TraceContext::s_current_method) {
      if (strstr(TraceContext::s_current_method->c_str(), m_method_filter) ==
          nullptr) {
        return;
      }
    }
    if (m_show_timestamps) {
      char buf[26];
      auto t = time(nullptr);
      ctime_r(&t, buf);
      buf[strlen(buf) - 1] = '\0';
      fprintf(m_file, "[%s] ", buf);
    }
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
  long m_level{0};
  std::array<long, N_TRACE_MODULES> m_traces;
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

std::unique_ptr<std::string> TraceContext::s_current_method;
