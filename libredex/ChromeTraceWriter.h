/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Writes a Chrome Trace Event format JSON file that can be loaded
// in chrome://tracing or Perfetto (https://ui.perfetto.dev/).
//
// Usage:
//   ChromeTraceWriter::init();        // as early as possible in main()
//   // ... after parsing args, if tracing is not wanted:
//   ChromeTraceWriter::disable();     // stops recording, frees events
//   // ... timers run, calling record() from their destructors ...
//   ChromeTraceWriter::write(path);   // at shutdown (if still enabled)
class ChromeTraceWriter {
 public:
  using clock = std::chrono::high_resolution_clock;
  using time_point = clock::time_point;

  // Enable tracing and capture the epoch (process start time).
  // Must be called in a single-threaded context (early in main()) before any
  // Timer is created, so that the epoch predates all events.
  static void init();

  // Disable tracing and discard any recorded events.
  static void disable();

  // Returns true if tracing is enabled.  This is the hot path checked on
  // every Timer destruction — it is a single pointer-null check with no
  // hidden mutex or function-local-static overhead.
  static bool enabled() {
    return s_enabled.load(std::memory_order_acquire) != nullptr;
  }

  // Record a completed timing event. Thread-safe.
  static void record(const std::string& name,
                     time_point start,
                     time_point end,
                     std::thread::id tid);

  // Write all recorded events to a JSON file in Chrome Trace format.
  static void write(const std::string& path);

 private:
  struct Event {
    std::string name;
    uint64_t ts_us; // start timestamp in microseconds since epoch
    uint64_t dur_us; // duration in microseconds
    uint64_t tid; // thread id
  };

  // Heap-allocated state that is intentionally never deleted, so that it
  // survives past static destructors.  This avoids the static destruction
  // order fiasco when exit() is called (e.g. --reflect-config) and other
  // statics (ConcurrentContainer mutexes) are destroyed before our state.
  struct State {
    std::mutex lock;
    std::vector<Event> events;
    time_point epoch;
  };

  // Non-null when tracing is active.  Points to the leaked State.
  // Checked on every Timer destruction as the fast-path guard.
  // NOLINTNEXTLINE(facebook-hte-NonPodStaticDeclaration)
  static std::atomic<State*> s_enabled;

  static State& get_state();
};
