/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <iostream>

#include "PowersetAbstractDomain.h"

namespace ssad_impl {

/*
 * The definition of an abstract value belonging to an abstract domain,
 * a sparse set data structure implemented using two arrays(vectors)
 * with fixed size, following the paper below:
 *
 * P. Briggs & L. Torczon. An Efficient Representation for
 * Sparse Sets. ACM Letters on Programming Languages and Systems,
 * 2(1-4):59-69,1993.
 */

class SparseSetValue final : public PowersetImplementation<
                                 uint16_t,
                                 const SparseSetValue&,
                                 SparseSetValue> {
 public:
  // Default constructor to pass sanity check in AbstractValue's destructor.
  SparseSetValue() : m_capacity(0), m_element_num(0) {}

  // Constructor that sets the maximum number of elements this set can hold.
  SparseSetValue(uint16_t max_size)
      : m_capacity(max_size),
        m_element_num(0),
        m_dense(max_size),
        m_sparse(max_size) {}

  void clear() override {
    m_element_num = 0;
  }

  const SparseSetValue& elements() const override {
    return *(this);
  }

  // Returning a vector that contains all the elements in the sparse set.
  // (for test use)
  std::vector<uint16_t> vals() const {
    return std::vector<uint16_t>(begin(), end());
  }

  AbstractValueKind kind() const override {
    return AbstractValueKind::Value;
  }

  // Checking if candidate is a member of the set.
  bool contains(const uint16_t& candidate) const override {
    if (candidate >= m_capacity) {
      return false;
    }
    uint16_t dense_idx = m_sparse[candidate];
    return dense_idx < m_element_num && m_dense[dense_idx] == candidate;
  }

  bool leq(const SparseSetValue& other) const override {
    if (m_element_num > other.m_element_num) {
      return false;
    }
    for (int i = 0; i < m_element_num; ++i) {
      if (!other.contains(m_dense[i])) {
        return false;
      }
    }
    return true;
  }

  bool equals(const SparseSetValue& other) const override {
    return (m_element_num == other.m_element_num) && this->leq(other);
  }

  // Adding elem to the set, if success return true,
  // return false otherwise.
  void add(const uint16_t& elem) override {
    if (elem < m_capacity) {
      uint16_t dense_idx = m_sparse[elem];
      uint16_t n = m_element_num;
      if (dense_idx >= m_element_num || m_dense[dense_idx] != elem) {
        m_sparse[elem] = n;
        m_dense[n] = elem;
        m_element_num = n + 1;
      }
    }
  }

  // Delete elem from the set, if success return true,
  // return false otherwise.
  void remove(const uint16_t& elem) override {
    if (elem < m_capacity) {
      uint16_t dense_idx = m_sparse[elem];
      uint16_t n = m_element_num;
      if (dense_idx < n && m_dense[dense_idx] == elem) {
        uint16_t last_elem = m_dense[n - 1];
        m_element_num = n - 1;
        m_dense[dense_idx] = last_elem;
        m_sparse[last_elem] = dense_idx;
      }
    }
  }

  std::vector<uint16_t>::iterator begin() {
    return m_dense.begin();
  }

  std::vector<uint16_t>::iterator end() {
    return std::next(m_dense.begin(), m_element_num);
  }

  std::vector<uint16_t>::const_iterator begin() const {
    return m_dense.cbegin();
  }

  std::vector<uint16_t>::const_iterator end() const {
    return std::next(m_dense.begin(), m_element_num);
  }

  AbstractValueKind join_with(const SparseSetValue& other) override {
    if (other.m_capacity > m_capacity) {
      m_dense.resize(other.m_capacity);
      m_sparse.resize(other.m_capacity);
      m_capacity = other.m_capacity;
    }
    for (auto e : other) {
      this->add(e);
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind widen_with(const SparseSetValue& other) override {
    return join_with(other);
  }

  AbstractValueKind meet_with(const SparseSetValue& other) override {
    for (auto it = this->begin(); it != this->end();) {
      if (!other.contains(*it)) {
        // If other doesn't contain this value
        // call remove() to remove the value at current position
        // remove() will fill this position with the last element
        // in the dense array, so we hold at the position to check
        // the value just filled in the next round.
        this->remove(*it);
      } else {
        // If other contains this value, we can
        // move on to the next position.
        ++it;
      }
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind narrow_with(const SparseSetValue& other) override {
    return meet_with(other);
  }

  size_t size() const override {
    return m_element_num;
  }

  friend std::ostream& operator<<(
      std::ostream& o,
      const SparseSetValue& value) {
    o << "[#" << value.size() << "]";
    const auto& elements = value.elements();
    o << "{";
    for (auto it = elements.begin(); it != elements.end();) {
      o << *it++;
      if (it != elements.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  uint16_t m_capacity;
  uint16_t m_element_num;
  std::vector<uint16_t> m_dense;
  std::vector<uint16_t> m_sparse;
  friend class SparseSetAbstractDomain;
};

} // namespace ssad_impl

/*
 * An implementation of abstract domain using sparse set data structure
 * used AbstractDomainScaffolding template to build the domain
 */

class SparseSetAbstractDomain final : public PowersetAbstractDomain<
                                          uint16_t,
                                          ssad_impl::SparseSetValue,
                                          const ssad_impl::SparseSetValue&,
                                          SparseSetAbstractDomain> {
 public:
  using Value = ssad_impl::SparseSetValue;

  SparseSetAbstractDomain()
      : PowersetAbstractDomain<
            uint16_t,
            Value,
            const Value&,
            SparseSetAbstractDomain>() {}

  SparseSetAbstractDomain(AbstractValueKind kind)
      : PowersetAbstractDomain<
            uint16_t,
            Value,
            const Value&,
            SparseSetAbstractDomain>(kind) {}

  explicit SparseSetAbstractDomain(uint16_t max_size) {
    this->set_to_value(Value(max_size));
  }

  static SparseSetAbstractDomain bottom() {
    return SparseSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static SparseSetAbstractDomain top() {
    return SparseSetAbstractDomain(AbstractValueKind::Top);
  }
};
