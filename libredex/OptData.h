/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Trace.h"
#include "Util.h"

// We cannibalize the TraceModule enum from Trace.h as an easy way to
// categorize opt data per pass.

// TODO (anwangster) LOG_NO_OPT, LOG_DEAD_CODE, etc.
// TODO (anwangster) Add DexClass/DexMethod/insns(?) as parameters.
bool log_enabled(TraceModule module);
void log_opt(TraceModule module, const char* fmt, ...);
#define LOG_OPT(module, fmt, ...)          \
  do {                                     \
    if (log_enabled(module)) {             \
      log_opt(module, fmt, ##__VA_ARGS__); \
    }                                      \
  } while (0)

// For thread-safe mapping.
// TODO (anwangster) Is the use of ConcurrentMaps better?
struct OptDataContext {
  explicit OptDataContext(const std::string& current_method) {
    s_current_method = &current_method;
  }
  ~OptDataContext() { s_current_method = nullptr; }

  thread_local static const std::string* s_current_method;
  static std::mutex s_opt_log_mutex;
};

// TODO (anwangster) Singleton = good/bad?
// If bad, use OptDataMapper only to write the opt data to a file.
class OptDataMapper {
 public:
  static OptDataMapper& get_instance() {
    static OptDataMapper instance;
    return instance;
  }
  OptDataMapper(OptDataMapper const&) = delete;
  void operator=(OptDataMapper const&) = delete;

  /**
   * For now, every log attempt succeeds.
   */
  bool log_enabled(TraceModule module);

  /**
   * Records the given message and attributes it to the given module.
   */
  void log_opt(TraceModule module, const char* fmt, va_list ap);

  /**
   * Writes the gathered optimization data in human-readable format.
   * For now, the data is just an echo back of whatever LOG_OPT gets called on.
   */
  void write_opt_data(const std::string& filename);

 private:
  // TODO (anwangster) Store logs per class/method/insn.
  // For now, we store per module.
  std::unordered_map<int /*TraceModule*/, std::string> m_module_id_name_map;
  std::unordered_map<int /*TraceModule*/, std::vector<std::string>>
      m_opt_trace_map;

  OptDataMapper() {
#define TM(x) m_module_id_name_map[static_cast<int>(x)] = #x;
    TMS
#undef TM
  }
};
