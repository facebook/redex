/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ChromeTraceWriter.h"

#include <array>
#include <fstream>
#include <functional>
#include <iostream>

// Zero-initialized at startup, no constructor needed.
std::atomic<ChromeTraceWriter::State*> ChromeTraceWriter::s_enabled{nullptr};

ChromeTraceWriter::State& ChromeTraceWriter::get_state() {
  // Intentionally leaked – must outlive every static destructor so that
  // record() and disable() remain safe when exit() triggers tear-down.
  static auto* state = new State();
  return *state;
}

// Must be called single-threaded (early in main(), before any Timer is
// created).  Sets up the epoch and enables collection.
void ChromeTraceWriter::init() {
  auto& s = get_state();
  s.epoch = clock::now();
  s_enabled.store(&s, std::memory_order_release);
}

void ChromeTraceWriter::disable() {
  s_enabled.store(nullptr, std::memory_order_release);
  auto& s = get_state();
  std::lock_guard<std::mutex> guard(s.lock);
  s.events.clear();
  s.events.shrink_to_fit();
}

void ChromeTraceWriter::record(const std::string& name,
                               time_point start,
                               time_point end,
                               std::thread::id tid) {
  auto* s = s_enabled.load(std::memory_order_acquire);
  if (s == nullptr) {
    return;
  }
  auto ts_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(start - s->epoch)
          .count());
  auto dur_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count());
  auto tid_hash = std::hash<std::thread::id>{}(tid);

  std::lock_guard<std::mutex> guard(s->lock);
  s->events.push_back({name, ts_us, dur_us, tid_hash});
}

namespace {
// Escape a string for JSON output. Only characters that need escaping per
// RFC 8259 are handled: backslash, double-quote, and control characters.
void write_json_string(std::ofstream& out, const std::string& s) {
  out << '"';
  for (char c : s) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        // Control character — emit as \u00XX.
        std::array<char, 8> buf;
        snprintf(
            buf.data(), buf.size(), "\\u%04x", static_cast<unsigned char>(c));
        out << buf.data();
      } else {
        out << c;
      }
      break;
    }
  }
  out << '"';
}
} // namespace

void ChromeTraceWriter::write(const std::string& path) {
  auto& s = get_state();
  if (s.events.empty()) {
    return;
  }

  std::ofstream out(path);
  if (!out) {
    std::cerr << "Warning: could not open chrome trace file: " << path << '\n';
    return;
  }

  out << '[';
  bool first = true;
  for (const auto& e : s.events) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << "{\"name\":";
    write_json_string(out, e.name);
    out << ",\"ph\":\"X\""
        << ",\"ts\":" << e.ts_us << ",\"dur\":" << e.dur_us << ",\"pid\":1"
        << ",\"tid\":" << e.tid << '}';
  }
  out << ']';
}
