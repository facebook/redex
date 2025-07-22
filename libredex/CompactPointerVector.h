/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A CompactPointerVector<> implements an expandable collection type similar to
 * std::vector<>, but specialized to hold only pointer values with pointed-to
 * value type alignment greater than 1, and optimized for storing 0 or 1
 * elements:
 * - sizeof(CompactPointerVector<>) is sizeof(void*)
 * - When empty, no additional space is allocated.
 * - When size() == 1, no additional space is allocated. The pointer value is
 *   stored inline.
 * - When size() > 1, a std::vector is allocated to hold the elements (a pointer
 *   to the vector stored with a tag). (We don't actively shrink that vector,
 *   except when it reaches size 0 or 1.)
 *
 * For efficiency, storing nullptr is not allowed.
 */

#pragma once

#include <boost/intrusive/pointer_plus_bits.hpp>
#include <type_traits>
#include <vector>

#include "Debug.h"

template <typename Ptr,
          typename =
              std::enable_if_t<std::is_pointer_v<Ptr> &&
                               (alignof(std::remove_pointer_t<Ptr>) > 1)>>
class CompactPointerVector {
 public:
  using iterator = Ptr*;
  using const_iterator = const Ptr*;

  CompactPointerVector() = default;

  CompactPointerVector(const CompactPointerVector& other) {
    if (other.many()) {
      m_data = make_data_vec(*other.as_vec());
      return;
    }
    m_data = other.m_data;
    // no bits to set
  }

  CompactPointerVector& operator=(const CompactPointerVector& other) {
    if (this == &other) {
      return *this;
    }
    clear(); // Release current resources
    if (other.many()) {
      m_data = make_data_vec(*other.as_vec());
      return *this;
    }
    m_data = other.m_data;
    // no bits to set
    return *this;
  }

  CompactPointerVector(CompactPointerVector&& other) noexcept {
    m_data = other.m_data;
    other.m_data = nullptr; // Reset the source to empty state
  }

  CompactPointerVector& operator=(CompactPointerVector&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    clear(); // Release current resources
    m_data = other.m_data;
    other.m_data = nullptr; // Reset the source to empty state
    return *this;
  }

  void push_back(Ptr ptr) {
    always_assert(ptr != nullptr);
    if (empty()) {
      // Transition from zero to one
      m_data = ptr;
      // no bits to set
      return;
    }
    if (one()) {
      // Transition from one to many
      m_data = make_data_vec(static_cast<Ptr>(m_data), ptr);
      return;
    }
    as_vec()->push_back(ptr);
  }

  void pop_back() {
    always_assert(!empty());
    if (one()) {
      m_data = nullptr;
      // no bits to set
      return;
    }
    Vec* vec = as_vec();
    vec->pop_back();
    if (vec->size() == 1) {
      m_data = vec->front();
      // no bits to set
      delete vec;
    }
  }

  void shrink_to_fit() {
    if (many()) {
      as_vec()->shrink_to_fit();
    }
  }

  Ptr& operator[](size_t idx) {
    // No bounds checking!
    if (one()) {
      // Only valid index is 0
      return reinterpret_cast<Ptr&>(m_data);
    }
    return (*as_vec())[idx];
  }

  const Ptr& operator[](size_t idx) const {
    // No bounds checking!
    if (one()) {
      // Only valid index is 0
      return reinterpret_cast<const Ptr&>(m_data);
    }
    return (*as_vec())[idx];
  }

  Ptr& at(size_t idx) {
    if (one()) {
      always_assert(idx == 0);
      return reinterpret_cast<Ptr&>(m_data);
    }
    return (*as_vec())[idx];
  }

  const Ptr& at(size_t idx) const {
    if (one()) {
      always_assert(idx == 0);
      return reinterpret_cast<const Ptr&>(m_data);
    }
    return as_vec()->at(idx);
  }

  Ptr& front() {
    always_assert(!empty());
    if (one()) {
      return reinterpret_cast<Ptr&>(m_data);
    }
    return as_vec()->front();
  }

  const Ptr& front() const {
    always_assert(!empty());
    if (one()) {
      return reinterpret_cast<const Ptr&>(m_data);
    }
    return as_vec()->front();
  }

  Ptr& back() {
    always_assert(!empty());
    if (one()) {
      return reinterpret_cast<Ptr&>(m_data);
    }
    return as_vec()->back();
  }

  const Ptr& back() const {
    always_assert(!empty());
    if (one()) {
      return reinterpret_cast<const Ptr&>(m_data);
    }
    return as_vec()->back();
  }

  iterator erase(iterator first, iterator last) {
    iterator b = begin();
    iterator e = end();
    always_assert(b <= first);
    always_assert(first <= last);
    always_assert(last <= e);
    if (first == last) {
      return first;
    }
    if (many()) {
      auto* vec = as_vec();
      auto it =
          vec->erase(vec->begin() + (first - b), vec->begin() + (last - b));
      // Transition to small state if size is 0 or 1
      size_t sz = vec->size();
      if (sz == 0) {
        m_data = nullptr;
        // no bits to set
        delete vec;
        return end();
      }
      if (sz == 1) {
        bool erased_to_end = it == vec->end();
        always_assert(erased_to_end || it == vec->begin());
        m_data = vec->front();
        // no bits to set
        delete vec;
        return erased_to_end ? end() : begin();
      }
      // Note that we don't bother shrinking the (many) vector, to keep
      // amortized costs in line with expectations
      return &*it;
    }
    always_assert(one());
    always_assert(first == b);
    always_assert(last == e);
    m_data = nullptr;
    // no bits to set
    return end();
  }

  size_t size() const {
    if (one()) {
      return 1;
    }
    if (empty()) {
      return 0;
    }
    return as_vec()->size();
  }

  void clear() {
    if (many()) {
      delete as_vec();
    }
    m_data = nullptr;
    // no bits to set
  }

  bool empty() const { return m_data == nullptr; }

  ~CompactPointerVector() { clear(); }

  iterator begin() {
    if (many()) {
      return as_vec()->data();
    }
    if (one()) {
      return reinterpret_cast<iterator>(&m_data);
    }
    return nullptr;
  }

  iterator end() {
    if (many()) {
      Vec* vec = as_vec();
      return vec->data() + vec->size();
    }
    if (one()) {
      return reinterpret_cast<iterator>(&m_data) + 1;
    }
    return nullptr;
  }

  const_iterator begin() const {
    if (many()) {
      return as_vec()->data();
    }
    if (one()) {
      return reinterpret_cast<const_iterator>(&m_data);
    }
    return nullptr;
  }

  const_iterator end() const {
    if (many()) {
      const Vec* vec = as_vec();
      return vec->data() + vec->size();
    }
    if (one()) {
      return reinterpret_cast<const_iterator>(&m_data) + 1;
    }
    return nullptr;
  }

  std::vector<Ptr> to_vector() const {
    if (many()) {
      return *as_vec();
    }
    if (one()) {
      return Vec{reinterpret_cast<Ptr>(m_data)};
    }
    return Vec{};
  }

 private:
  static const size_t kEmptyOrOne = 0;
  static const size_t kMany = 1;
  using Vec = std::vector<Ptr>;
  using Accessor = boost::intrusive::pointer_plus_bits<void*, 1>;

  bool one() const {
    return m_data != nullptr && Accessor::get_bits(m_data) == kEmptyOrOne;
  }

  bool many() const {
    return m_data != nullptr && Accessor::get_bits(m_data) == kMany;
  }

  const Vec* as_vec() const {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(m_data));
  }

  Vec* as_vec() {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(m_data));
  }

  template <typename... Args>
  static void* make_data_vec(Args&&... args) {
    void* new_data = new Vec{std::forward<Args>(args)...};
    Accessor::set_bits(new_data, kMany);
    return new_data;
  }

  void* m_data{nullptr};
};
