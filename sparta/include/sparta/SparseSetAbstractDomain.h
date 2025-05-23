/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iterator>
#include <ostream>
#include <vector>

#include <sparta/PowersetAbstractDomain.h>

namespace sparta {

namespace ssad_impl {

/*
 * An implementation of a powerset abstract domain based on the sparse data
 * structure described in the following paper:
 *
 * P. Briggs & L. Torczon. An Efficient Representation for Sparse Sets. ACM
 * Letters on Programming Languages and Systems, 2(1-4):59-69,1993.
 *
 * This powerset domain can only handle elements that are unsigned integers
 * belonging to a fixed-size universe {0, ..., max_size-1}.
 */
template <typename IntegerType>
class SparseSetValue final
    : public PowersetImplementation<IntegerType,
                                    std::vector<IntegerType>,
                                    SparseSetValue<IntegerType>> {
 public:
  using iterator = typename std::vector<IntegerType>::iterator;
  using const_iterator = typename std::vector<IntegerType>::const_iterator;

  // This constructor is defined solely to satisfy the requirement that an
  // AbstractDomain must be default-constructible. It shouldn't be used in
  // practice.
  SparseSetValue() : m_capacity(0), m_element_num(0) {}

  // Returns an empty set over a universe of the given size.
  explicit SparseSetValue(size_t max_size)
      : m_capacity(max_size),
        m_element_num(0),
        m_dense(max_size),
        m_sparse(max_size) {}

  void clear() { m_element_num = 0; }

  // Returns a vector that contains all the elements in the sparse set.
  std::vector<IntegerType> elements() const {
    return std::vector<IntegerType>(begin(), end());
  }

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool contains(const IntegerType& element) const {
    if (element >= m_capacity) {
      return false;
    }
    size_t dense_idx = m_sparse[element];
    return dense_idx < m_element_num && m_dense[dense_idx] == element;
  }

  bool leq(const SparseSetValue& other) const {
    if (m_element_num > other.m_element_num) {
      return false;
    }
    for (size_t i = 0; i < m_element_num; ++i) {
      if (!other.contains(m_dense[i])) {
        return false;
      }
    }
    return true;
  }

  bool equals(const SparseSetValue& other) const {
    return (m_element_num == other.m_element_num) && this->leq(other);
  }

  void add(const IntegerType& element) {
    if (element < m_capacity) {
      size_t dense_idx = m_sparse[element];
      size_t n = m_element_num;
      if (dense_idx >= m_element_num || m_dense[dense_idx] != element) {
        m_sparse[element] = n;
        m_dense[n] = element;
        m_element_num = n + 1;
      }
    }
  }

  void add(IntegerType&& element) { add(element); }

  void remove(const IntegerType& element) {
    if (element < m_capacity) {
      size_t dense_idx = m_sparse[element];
      size_t n = m_element_num;
      if (dense_idx < n && m_dense[dense_idx] == element) {
        IntegerType last_element = m_dense[n - 1];
        m_element_num = n - 1;
        m_dense[dense_idx] = last_element;
        m_sparse[last_element] = dense_idx;
      }
    }
  }

  template <typename Predicate>
  void filter(Predicate&& predicate) {
    for (auto it = begin(); it != end();) {
      if (predicate(*it)) {
        // If predicate is true for this element, we just move on to the next
        // position.
        ++it;
      } else {
        // If predicate returns false for this element, we remove it using the
        // current position. The function remove() will fill this position with
        // the last element in the dense array.
        remove(*it);
      }
    }
  }

  iterator begin() { return m_dense.begin(); }

  iterator end() { return std::next(m_dense.begin(), m_element_num); }

  const_iterator begin() const { return m_dense.begin(); }

  const_iterator end() const {
    return std::next(m_dense.begin(), m_element_num);
  }

  AbstractValueKind join_with(const SparseSetValue& other) {
    if (other.m_capacity > m_capacity) {
      m_dense.resize(other.m_capacity);
      m_sparse.resize(other.m_capacity);
      m_capacity = other.m_capacity;
    }
    for (IntegerType e : other) {
      add(e);
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind widen_with(const SparseSetValue& other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const SparseSetValue& other) {
    for (auto it = begin(); it != end();) {
      if (!other.contains(*it)) {
        // If other doesn't contain this element, we remove it using the current
        // position. The function remove() will fill this position with the last
        // element in the dense array.
        remove(*it);
      } else {
        // If other contains this element, we just move on to the next position.
        ++it;
      }
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind narrow_with(const SparseSetValue& other) {
    return meet_with(other);
  }

  AbstractValueKind difference_with(const SparseSetValue& other) {
    for (auto it = begin(); it != end();) {
      if (other.contains(*it)) {
        remove(*it);
      } else {
        ++it;
      }
    }
    return AbstractValueKind::Value;
  }

  bool empty() const { return m_element_num == 0; }

  size_t size() const { return m_element_num; }

  friend std::ostream& operator<<(std::ostream& o,
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
  size_t m_capacity;
  size_t m_element_num;
  std::vector<IntegerType> m_dense;
  std::vector<size_t> m_sparse;
};

} // namespace ssad_impl

/*
 * We defined a powerset abstract domain based on the sparse set data structure.
 */
template <typename IntegerType>
class SparseSetAbstractDomain final
    : public PowersetAbstractDomain<IntegerType,
                                    ssad_impl::SparseSetValue<IntegerType>,
                                    std::vector<IntegerType>,
                                    SparseSetAbstractDomain<IntegerType>> {
 public:
  using Value = ssad_impl::SparseSetValue<IntegerType>;

  static_assert(std::is_unsigned_v<IntegerType>,
                "IntegerType is not an unsigned arihmetic type");
  static_assert(sizeof(IntegerType) <= sizeof(size_t),
                "IntegerType is too large");

  SparseSetAbstractDomain()
      : PowersetAbstractDomain<IntegerType,
                               Value,
                               std::vector<IntegerType>,
                               SparseSetAbstractDomain>() {}

  explicit SparseSetAbstractDomain(AbstractValueKind kind)
      : PowersetAbstractDomain<IntegerType,
                               Value,
                               std::vector<IntegerType>,
                               SparseSetAbstractDomain>(kind) {}

  explicit SparseSetAbstractDomain(IntegerType max_size) {
    this->set_to_value(Value(max_size));
  }

  static SparseSetAbstractDomain bottom() {
    return SparseSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static SparseSetAbstractDomain top() {
    return SparseSetAbstractDomain(AbstractValueKind::Top);
  }
};

} // namespace sparta
