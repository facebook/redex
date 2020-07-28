/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Trace.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "Macros.h"

namespace {

struct Tracer {

  bool m_show_timestamps{false};
  bool m_show_tracemodule{false};
  const char* m_method_filter;
  std::unordered_map<int /*TraceModule*/, std::string> m_module_id_name_map;

  Tracer() {
    const char* traceenv = getenv("TRACE");
    const char* envfile = getenv("TRACEFILE");
    const char* show_timestamps = getenv("SHOW_TIMESTAMPS");
    const char* show_tracemodule = getenv("SHOW_TRACEMODULE");
    m_method_filter = getenv("TRACE_METHOD_FILTER");
    if (!traceenv) {
      return;
    }

    std::cerr << "Trace settings:" << std::endl;
    std::cerr << "TRACEFILE=" << (envfile == nullptr ? "" : envfile)
              << std::endl;
    std::cerr << "SHOW_TIMESTAMPS="
              << (show_timestamps == nullptr ? "" : show_timestamps)
              << std::endl;
    std::cerr << "SHOW_TRACEMODULE="
              << (show_tracemodule == nullptr ? "" : show_tracemodule)
              << std::endl;
    std::cerr << "TRACE_METHOD_FILTER="
              << (m_method_filter == nullptr ? "" : m_method_filter)
              << std::endl;

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

  void trace(TraceModule module,
             int level,
             bool suppress_newline,
             const char* fmt,
             va_list ap) {
    if (m_method_filter && TraceContext::s_current_method != nullptr) {
      if (strstr(TraceContext::s_current_method->c_str(), m_method_filter) ==
          nullptr) {
        return;
      }
    }
    std::lock_guard<std::mutex> guard(TraceContext::s_trace_mutex);
    if (m_show_timestamps) {
      auto t = std::time(nullptr);
      struct tm local_tm;
#if IS_WINDOWS
      localtime_s(&local_tm, &t);
#else
      localtime_r(&t, &local_tm);
#endif
      std::array<char, 40> buf;
      std::strftime(buf.data(), sizeof(buf), "%c", &local_tm);
      fprintf(m_file, "[%s]", buf.data());
      if (!m_show_tracemodule) {
        fprintf(m_file, " ");
      }
    }
    if (m_show_tracemodule) {
      fprintf(m_file, "[%s:%d] ", m_module_id_name_map[module].c_str(), level);
    }
    vfprintf(m_file, fmt, ap);
    if (!suppress_newline) {
      fprintf(m_file, "\n");
    }
    fflush(m_file);
  }

 private:
  void init_trace_modules(const char* traceenv) {
    std::unordered_map<std::string, int> module_id_map{{
#define TM(x) {std::string(#x), x},
        TMS
#undef TM
    }};

    char* tracespec = strdup(traceenv);
    const char* sep = ",: ";
    const char* module = nullptr;
    for (const char* tok = strtok(tracespec, sep); tok != nullptr;
         tok = strtok(nullptr, sep)) {
      auto level = strtol(tok, nullptr, 10);
      if (level) {
        if (module) {
          if (module_id_map.count(module) == 0) {
            if (strcmp(module, "REDEX") == 0) {
              continue; // Ignore REDEX.
            }
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
      fprintf(stderr, "Unable to open TRACEFILE, falling back to stderr\n");
      m_file = stderr;
    }
  }

 private:
  FILE* m_file{nullptr};
  long m_level{0};
  std::array<long, N_TRACE_MODULES> m_traces;
};

static Tracer tracer;
} // namespace

#ifndef NDEBUG
bool traceEnabled(TraceModule module, int level) {
  return tracer.traceEnabled(module, level);
}
#endif

void trace(TraceModule module,
           int level,
           bool suppress_newline,
           const char* fmt,
           ...) {
  va_list ap;
  va_start(ap, fmt);
  tracer.trace(module, level, suppress_newline, fmt, ap);
  va_end(ap);
}

thread_local const std::string* TraceContext::s_current_method = nullptr;
std::mutex TraceContext::s_trace_mutex;
