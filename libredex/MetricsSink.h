/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Debug.h"

/*
 * Receives hierarchically scoped metrics. The base owns the scope stack and
 * assembles the dotted key, so an implementation only has to say where a
 * fully-qualified key and its value go.
 *
 * Code that merely reports numbers should take this rather than
 * `ScopedMetrics`, whose only constructor requires a `PassManager`.
 */
class MetricsSink {
 public:
  MetricsSink() = default;
  virtual ~MetricsSink() = default;

  MetricsSink(const MetricsSink&) = delete;
  MetricsSink& operator=(const MetricsSink&) = delete;

  // Keeps a scope segment pushed for as long as it is alive.
  struct Scope {
    explicit Scope(MetricsSink* parent) : parent(parent) {}
    ~Scope() {
      if (parent != nullptr) {
        parent->pop();
      }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    Scope(Scope&& other) noexcept : parent(other.parent) {
      other.parent = nullptr;
    }
    Scope& operator=(Scope&& other) noexcept {
      if (parent != nullptr) {
        parent->pop();
      }
      parent = other.parent;
      other.parent = nullptr;
      return *this;
    }

    MetricsSink* parent;
  };

  Scope scope(std::string key) {
    m_segments.emplace_back(std::move(key));
    return Scope(this);
  }

  // Template method to accept different arithmetic types
  template <typename T>
  typename std::enable_if_t<std::is_arithmetic_v<T>, void> set_metric(
      const std::string_view& key, T value) {
    report_metric(qualify(key), static_cast<int64_t>(value));
  }

  // Specialization for atomic types
  template <typename T>
  void set_metric(const std::string_view& key, const std::atomic<T>& value) {
    static_assert(std::is_arithmetic_v<T>, "T must be an arithmetic type");
    report_metric(qualify(key),
                  static_cast<int64_t>(value.load(std::memory_order_relaxed)));
  }

 protected:
  // Receives the key with every enclosing scope segment already prefixed.
  virtual void report_metric(const std::string& key, int64_t value) = 0;

 private:
  std::string qualify(const std::string_view& key) const {
    std::string full_key = cur_path();
    if (!full_key.empty()) {
      full_key.append(".");
    }
    full_key.append(key);
    return full_key;
  }

  std::string cur_path() const {
    std::string ret;
    for (const auto& s : m_segments) {
      if (!ret.empty()) {
        ret.append(".");
      }
      ret.append(s);
    }
    return ret;
  }

  void pop() {
    redex_assert(!m_segments.empty());
    m_segments.pop_back();
  }

  std::vector<std::string> m_segments;
};
