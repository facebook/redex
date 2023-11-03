/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iterator>
#include <optional>

template <class T>
struct singleton_iterator {
  using iterator_category = std::forward_iterator_tag;
  using value_type = T;
  using difference_type = bool;
  using pointer = T*;
  using reference = T&;

  explicit singleton_iterator(T& value) : m_value(&value) {}
  singleton_iterator() : m_value(nullptr) {}

  singleton_iterator& operator++() {
    always_assert(m_value);
    m_value = nullptr;
    return *this;
  }

  singleton_iterator operator++(int) {
    auto result = *this;
    ++(*this);
    return result;
  }

  reference operator*() const {
    always_assert(m_value);
    return *m_value;
  }

  pointer operator->() const { return &(this->operator*()); }

  bool operator==(const singleton_iterator<T>& that) const {
    return m_value == that.m_value;
  }
  bool operator!=(const singleton_iterator<T>& that) const {
    return m_value != that.m_value;
  }

 private:
  pointer m_value;
};

template <class T>
struct singleton_iterable {
  explicit singleton_iterable(T& value) : m_value(value) {}
  singleton_iterator<T> begin() { return singleton_iterator<T>(m_value); }
  singleton_iterator<T> end() { return singleton_iterator<T>(); }
  T& m_value;
};
