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
  bool m_show_tracemodule{false};
  const char* m_method_filter;
  std::unordered_map<int/*TraceModule*/, std::string> m_module_id_name_map;

  Tracer() {
    const char* traceenv = getenv("TRACE");
    const char* envfile = getenv("TRACEFILE");
    const char* show_timestamps = getenv("SHOW_TIMESTAMPS");
    const char* show_tracemodule = getenv("SHOW_TRACEMODULE");
    m_method_filter = getenv("TRACE_METHOD_FILTER");
    if (!traceenv) {
      return;
    }
    init_trace_modules(traceenv);
    init_trace_file(envfile);

    if (show_timestamps) {
      m_show_timestamps = true;
    }
    if (show_tracemodule) {
      m_show_tracemodule = true;
    }

#define TM(x) m_module_id_name_map[static_cast<int>(x)] = #x;
    TMS
#undef TM
  }

  ~Tracer() {
    if (m_file != nullptr && m_file != stderr) {
      fclose(m_file);
    }
  }

  bool traceEnabled(TraceModule module, int level) {
    return level <= m_level || level <= m_traces[module];
  }

  void trace(TraceModule module, int level, const char* fmt, va_list ap) {
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
      fprintf(m_file, "[%s]", buf);
      if (!m_show_tracemodule) {
        fprintf(m_file, " ");
      }
    }
    if (m_show_tracemodule) {
      fprintf(m_file, "[%s:%d] ", m_module_id_name_map[module].c_str(), level);
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
      m_file = stderr;
      return;
    }
    try {
      // If redex-all is called from redex.py, the tracefile has been created
      // already. And TRACEFILE is replaced by the file descriptor instead.
      // Refer to update_trace_file in pyredex/logger.py.
      auto fd = std::stoi(envfile);
      m_file = fdopen(fd, "w");
    } catch (std::invalid_argument&) {
      // Not an integer file descriptor; real file name.
      m_file = fopen(envfile, "w");
    }
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

void trace(TraceModule module, int level, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  tracer.trace(module, level, fmt, ap);
  va_end(ap);
}

std::unique_ptr<std::string> TraceContext::s_current_method;
