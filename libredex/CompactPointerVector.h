/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A CompactPointerVector<> implements an expandable collection type similar to
 * std::vector<>, but specialized to hold only pointer values with pointed-to
 * value type alignment greater than 2, and optimized for storing 0, 1, or 2
 * elements:
 * - sizeof(CompactPointerVector<>) is sizeof(void*)
 * - When empty, no additional space is allocated.
 * - When size() == 1, no additional space is allocated. The pointer value is
 *   stored inline.
 * - When size() == 2, a std::array<Ptr, 2> is allocated
 * - When size() > 2, a std::vector is allocated to hold the elements. (We don't
 *   actively shrink that vector, except when it reaches size 0, 1, or 2.)
 *
 * For efficiency, storing nullptr is not allowed.
 */

#pragma once

#include <array>
#include <boost/intrusive/pointer_plus_bits.hpp>
#include <type_traits>
#include <vector>

#include "Debug.h"

template <typename Ptr,
          typename =
              std::enable_if_t<std::is_pointer_v<Ptr> &&
                               (alignof(std::remove_pointer_t<Ptr>) > 2)>>
class CompactPointerVector {
 public:
  using iterator = Ptr*;
  using const_iterator = const Ptr*;

  CompactPointerVector() = default;

  CompactPointerVector(const CompactPointerVector& other) {
    m_data = other.clone_data();
  }

  CompactPointerVector& operator=(const CompactPointerVector& other) {
    if (this == &other) {
      return *this;
    }
    clear(); // Release current resources
    m_data = other.clone_data();
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
      // Transition from one to two
      m_data = make_data_arr2(static_cast<Ptr>(m_data), ptr);
      return;
    }
    if (two()) {
      // Transition from two to many
      auto* arr2 = as_arr2();
      m_data = make_data_vec((*arr2)[0], (*arr2)[1], ptr);
      delete arr2;
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
    if (two()) {
      auto* arr2 = as_arr2();
      m_data = (*arr2)[0];
      delete arr2;
      return;
    }
    Vec* vec = as_vec();
    vec->pop_back();
    if (vec->size() == 2) {
      m_data = make_data_arr2(vec->front(), vec->back());
      delete vec;
      return;
    }
    if (vec->size() == 1) {
      m_data = vec->front();
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
    return *(begin() + idx);
  }

  const Ptr& operator[](size_t idx) const {
    // No bounds checking!
    return *(begin() + idx);
  }

  Ptr& at(size_t idx) {
    always_assert(idx < size());
    return *(begin() + idx);
  }

  const Ptr& at(size_t idx) const {
    always_assert(idx < size());
    return *(begin() + idx);
  }

  Ptr& front() {
    always_assert(!empty());
    return *begin();
  }

  const Ptr& front() const {
    always_assert(!empty());
    return *begin();
  }

  Ptr& back() {
    always_assert(!empty());
    return *(end() - 1);
  }

  const Ptr& back() const {
    always_assert(!empty());
    return *(end() - 1);
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
    auto idx = first - b;
    if (many()) {
      auto* vec = as_vec();
      (void)vec->erase(vec->begin() + idx, vec->begin() + (last - b));
      // Transition to small state if size is 0, 1, or 2
      size_t sz = vec->size();
      if (sz == 0) {
        m_data = nullptr;
        // no bits to set
        delete vec;
      } else if (sz == 1) {
        m_data = vec->front();
        // no bits to set
        delete vec;
      } else if (sz == 2) {
        m_data = make_data_arr2(vec->front(), vec->back());
        delete vec;
      }
      // Note that we don't bother shrinking the (many) vector, to keep
      // amortized costs in line with expectations
    } else if (two()) {
      auto arr2 = as_arr2();
      if (last - first == 2) {
        m_data = nullptr;
        // no bits to set
      } else {
        always_assert(last - first == 1);
        m_data = (*arr2)[1 - idx];
        // no bits to set
      }
      delete arr2;
    } else {
      always_assert(one());
      always_assert(first == b);
      always_assert(last == e);
      m_data = nullptr;
      // no bits to set
    }
    return begin() + idx;
  }

  size_t size() const {
    return empty() ? 0 : one() ? 1 : two() ? 2 : as_vec()->size();
  }

  size_t capacity() const {
    return empty() ? 0 : one() ? 1 : two() ? 2 : as_vec()->capacity();
  }

  void clear() {
    if (two()) {
      delete as_arr2();
    } else if (many()) {
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
    if (two()) {
      return as_arr2()->data();
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
    if (two()) {
      return as_arr2()->data() + 2;
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
    if (two()) {
      return as_arr2()->data();
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
    if (two()) {
      return as_arr2()->data() + 2;
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
    if (two()) {
      auto* arr2 = as_arr2();
      return Vec{(*arr2)[0], (*arr2)[1]};
    }
    if (one()) {
      return Vec{reinterpret_cast<Ptr>(m_data)};
    }
    return Vec{};
  }

 private:
  static const size_t kEmptyOrOne = 0;
  static const size_t kTwo = 1;
  static const size_t kMany = 2;
  using Vec = std::vector<Ptr>;
  using Arr2 = std::array<Ptr, 2>;
  using Accessor = boost::intrusive::pointer_plus_bits<void*, 2>;

  bool one() const {
    return m_data != nullptr && Accessor::get_bits(m_data) == kEmptyOrOne;
  }

  bool two() const { return Accessor::get_bits(m_data) == kTwo; }

  bool many() const { return Accessor::get_bits(m_data) == kMany; }

  const Vec* as_vec() const {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(m_data));
  }

  Vec* as_vec() {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(m_data));
  }

  const Arr2* as_arr2() const {
    always_assert(two());
    return static_cast<Arr2*>(Accessor::get_pointer(m_data));
  }

  Arr2* as_arr2() {
    always_assert(two());
    return static_cast<Arr2*>(Accessor::get_pointer(m_data));
  }

  template <typename... Args>
  static void* make_data_vec(Args&&... args) {
    void* new_data = new Vec{std::forward<Args>(args)...};
    Accessor::set_bits(new_data, kMany);
    return new_data;
  }

  template <typename... Args>
  static void* make_data_arr2(Args&&... args) {
    void* new_data = new Arr2{std::forward<Args>(args)...};
    Accessor::set_bits(new_data, kTwo);
    return new_data;
  }

  void* clone_data() const {
    if (many()) {
      return make_data_vec(*as_vec());
    }
    if (two()) {
      return make_data_arr2(*as_arr2());
    }
    return m_data;
  }

  void* m_data{nullptr};
};
