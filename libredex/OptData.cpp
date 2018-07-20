/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptData.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "DexClass.h"
#include "Trace.h"

bool log_enabled(TraceModule module) {
  return OptDataMapper::get_instance().log_enabled(module);
}

void log_opt(TraceModule module, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  // TODO (anwangster)
  // Is there too much overhead to get the instance for every log attempt?
  // Is it better to do as Trace does and keep a static struct around?
  OptDataMapper::get_instance().log_opt(module, fmt, ap);
  va_end(ap);
}

bool OptDataMapper::log_enabled(TraceModule module) {
  // TODO (anwangster)
  // Add in class/method/insn filter here and call in log_opt.
  return true;
}

void OptDataMapper::log_opt(TraceModule module, const char* fmt, va_list ap) {
  std::lock_guard<std::mutex> guard(OptDataContext::s_opt_log_mutex);
  va_list ap_copy;
  va_copy(ap_copy, ap);
  size_t size = std::vsnprintf(nullptr, 0, fmt, ap);
  // +1 for null terminator
  std::vector<char> buf(size + 1);
  std::vsprintf(buf.data(), fmt, ap_copy);
  m_opt_trace_map[module].emplace_back(buf.data());
  va_end(ap_copy);
}

void OptDataMapper::write_opt_data(const std::string& filename) {
  std::ofstream ofs(filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  for (size_t i = 0; i < N_TRACE_MODULES; ++i) {
    for (const auto& opt_datum : m_opt_trace_map[i]) {
      ofs << opt_datum.c_str();
    }
  }
}

thread_local const std::string* OptDataContext::s_current_method = nullptr;
std::mutex OptDataContext::s_opt_log_mutex;
