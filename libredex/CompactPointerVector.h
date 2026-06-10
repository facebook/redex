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
 */

#pragma once

#include <array>
#include <bit>
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
    other.m_data = make_data_none();
  }

  CompactPointerVector& operator=(CompactPointerVector&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    clear(); // Release current resources
    m_data = other.m_data;
    other.m_data = make_data_none();
    return *this;
  }

  void push_back(Ptr ptr) {
    if (empty()) {
      // Transition from zero to one
      m_data = ptr;
      // no bits to set
      return;
    }
    if (one()) {
      // Transition from one to two
      m_data = make_data_arr2(m_data, ptr);
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
      m_data = make_data_none();
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
        m_data = make_data_none();
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
        m_data = make_data_none();
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
      m_data = make_data_none();
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
    m_data = make_data_none();
  }

  bool empty() const { return Accessor::get_bits(as_void()) == kNone; }

  bool one() const { return Accessor::get_bits(as_void()) == kOne; }

  bool two() const { return Accessor::get_bits(as_void()) == kTwo; }

  bool many() const { return Accessor::get_bits(as_void()) == kMany; }

  ~CompactPointerVector() { clear(); }

  iterator begin() {
    if (many()) {
      return as_vec()->data();
    }
    if (two()) {
      return as_arr2()->data();
    }
    if (one()) {
      return &m_data;
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
      return &m_data + 1;
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
      return &m_data;
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
      return &m_data + 1;
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
      return Vec{m_data};
    }
    return Vec{};
  }

 private:
  static const size_t kOne = 0;
  static const size_t kNone = 1;
  static const size_t kTwo = 2;
  static const size_t kMany = 3;
  using Vec = std::vector<Ptr>;
  using Arr2 = std::array<Ptr, 2>;
  using Accessor = boost::intrusive::pointer_plus_bits<void*, 2>;

  // The storage word reinterpreted as the tagged `void*` understood by
  // `Accessor` (and used by the two/many heap-pointer states). `std::bit_cast`
  // is the standards-compliant way to reinterpret the object representation
  // between `Ptr` and `void*`.
  void* as_void() const { return std::bit_cast<void*>(m_data); }

  const Vec* as_vec() const {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(as_void()));
  }

  Vec* as_vec() {
    always_assert(many());
    return static_cast<Vec*>(Accessor::get_pointer(as_void()));
  }

  const Arr2* as_arr2() const {
    always_assert(two());
    return static_cast<Arr2*>(Accessor::get_pointer(as_void()));
  }

  Arr2* as_arr2() {
    always_assert(two());
    return static_cast<Arr2*>(Accessor::get_pointer(as_void()));
  }

  template <typename... Args>
  static Ptr make_data_vec(Args&&... args) {
    void* new_data = new Vec{std::forward<Args>(args)...};
    Accessor::set_bits(new_data, kMany);
    return std::bit_cast<Ptr>(new_data);
  }

  template <typename... Args>
  static Ptr make_data_arr2(Args&&... args) {
    void* new_data = new Arr2{std::forward<Args>(args)...};
    Accessor::set_bits(new_data, kTwo);
    return std::bit_cast<Ptr>(new_data);
  }

  static Ptr make_data_none() {
    void* new_data = nullptr;
    Accessor::set_bits(new_data, kNone);
    return std::bit_cast<Ptr>(new_data);
  }

  Ptr clone_data() const {
    if (many()) {
      return make_data_vec(*as_vec());
    }
    if (two()) {
      return make_data_arr2(*as_arr2());
    }
    return m_data;
  }

  // The storage word holds one of:
  //  - the empty/none sentinel (low bits == kNone);
  //  - in the `one` state, the single inline element itself (low bits == kOne,
  //    i.e. the element's own aligned address); or
  //  - in the two/many states, a tagged heap `Arr2*`/`Vec*` (low bits encode
  //    the state).
  //
  // It is declared as `Ptr` (not `void*`) precisely so that begin()/end() can
  // hand out a real `Ptr*` into the inline one-element storage. Returning a
  // `reinterpret_cast<Ptr*>(&m_data)` from a `void*` member instead reads a
  // `void*` object through a `Ptr` lvalue, a strict-aliasing violation that
  // clang-21's stricter TBAA miscompiled into a wild `edge->src()` crash in
  // ControlFlowGraph::move_edge. The tagged-`void*` interpretation used by the
  // Accessor and the heap states is recovered with `std::bit_cast` (see
  // `as_void()` and `make_data_*`), which reinterprets the object
  // representation in a fully standards-compliant way.
  Ptr m_data{make_data_none()};
};
