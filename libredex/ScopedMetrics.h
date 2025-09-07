/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <string>
#include <type_traits>
#include <vector>

#include "Debug.h"
#include "PassManager.h"

class ScopedMetrics {
 public:
  explicit ScopedMetrics(PassManager& pm) : m_pm(pm) {}

  struct Scope {
    explicit Scope(ScopedMetrics* parent) : parent(parent) {}
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

    ScopedMetrics* parent;
  };

  Scope scope(std::string key) {
    m_segments.emplace_back(std::move(key));
    return Scope(this);
  }

  // Template method to accept different arithmetic types
  template <typename T>
  typename std::enable_if_t<std::is_arithmetic_v<T>, void> set_metric(
      const std::string_view& key, T value) {
    auto full_key = cur_path(m_segments);
    if (!full_key.empty()) {
      full_key.append(".");
    }
    full_key.append(key);
    m_pm.set_metric(full_key, value);
  }

  // Specialization for atomic types
  template <typename T>
  void set_metric(const std::string_view& key, const std::atomic<T>& value) {
    static_assert(std::is_arithmetic_v<T>, "T must be an arithmetic type");
    auto full_key = cur_path(m_segments);
    if (!full_key.empty()) {
      full_key.append(".");
    }
    full_key.append(key);
    m_pm.set_metric(full_key, value);
  }

 private:
  std::string cur_path(const std::vector<std::string>& segments) const {
    std::string ret;
    for (const auto& s : segments) {
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
  PassManager& m_pm;

  friend struct Scope;
};
