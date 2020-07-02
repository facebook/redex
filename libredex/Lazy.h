/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

template <typename T>
// A convenient helper class for lazy initialization.
// This class is not thread-safe.
class Lazy {
 public:
  Lazy() = delete;
  Lazy(const Lazy&) = delete;
  Lazy& operator=(const Lazy&) = delete;
  explicit Lazy(const std::function<T()>& creator)
      : m_creator([creator] { return std::make_unique<T>(creator()); }) {}
  explicit Lazy(const std::function<std::unique_ptr<T>()>& creator)
      : m_creator(creator) {}
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  operator bool() { return !!m_value; }
  T& operator*() {
    init();
    return *m_value;
  }
  T* operator->() {
    init();
    return m_value.get();
  }

 private:
  std::function<std::unique_ptr<T>()> m_creator;
  std::unique_ptr<T> m_value;
  void init() {
    if (!m_value) {
      m_value = m_creator();
      // Release whatever memory is asssociated with creator
      m_creator = std::function<std::unique_ptr<T>()>();
    }
  }
};
template <class Key,
          class T,
          class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>,
          class Allocator = std::allocator<std::pair<const Key, T>>>
// A convenient helper class for lazily initialized maps.
// This class is not thread-safe.
class LazyUnorderedMap {
 public:
  LazyUnorderedMap() = delete;
  LazyUnorderedMap(const LazyUnorderedMap&) = delete;
  LazyUnorderedMap& operator=(const LazyUnorderedMap&) = delete;
  explicit LazyUnorderedMap(std::function<T(const Key& key)> creator)
      : m_creator(std::move(creator)) {}
  T& operator[](const Key key) {
    auto it = m_map.find(key);
    if (it == m_map.end()) {
      it = m_map.emplace(key, m_creator(key)).first;
    }
    return it->second;
  }

 private:
  std::function<T(const Key& key)> m_creator;
  std::unordered_map<Key, T, Hash, KeyEqual, Allocator> m_map;
};
