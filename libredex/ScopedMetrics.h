/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include "Debug.h"

class PassManager;

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

  // In cpp to avoid `PassManager.h` include.
  void set_metric(const std::string_view& key, int64_t value);

 private:
  void pop() {
    redex_assert(!m_segments.empty());
    m_segments.pop_back();
  }

  std::vector<std::string> m_segments;
  PassManager& m_pm;

  friend struct Scope;
};
