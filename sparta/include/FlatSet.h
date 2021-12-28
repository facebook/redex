/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <vector>

#include "PatriciaTreeUtil.h"

namespace sparta {

/*
 * Represents a set implemented with a sorted vector.
 *
 * It is similar to `boost::container::flat_set` but provides set operations
 * such as union, intersection and difference, using the same interface as
 * `PatriciaTreeSet`.
 */
template <typename Element,
          typename Compare = std::less<Element>,
          typename Equal = std::equal_to<Element>>
class FlatSet final {
 public:
  // C++ container concept member types
  using iterator = typename std::vector<Element>::const_iterator;
  using const_iterator = iterator;
  using value_type = Element;
  using difference_type = std::ptrdiff_t;
  using size_type = std::size_t;
  using const_reference = const Element&;
  using const_pointer = const Element*;

  FlatSet() = default;

  explicit FlatSet(std::initializer_list<Element> l) {
    for (const Element& x : l) {
      insert(x);
    }
  }

  template <typename InputIterator>
  FlatSet(InputIterator first, InputIterator last) {
    for (auto it = first; it != last; ++it) {
      insert(*it);
    }
  }

  bool empty() const { return m_vector.empty(); }

  std::size_t size() const { return m_vector.size(); }

  std::size_t max_size() const { return m_vector.max_size(); }

  iterator begin() const { return m_vector.begin(); }

  iterator end() const { return m_vector.end(); }

  bool contains(const Element& key) const {
    auto it =
        std::lower_bound(m_vector.begin(), m_vector.end(), key, Compare());
    return it != m_vector.end() && Equal()(*it, key);
  }

  bool is_subset_of(const FlatSet& other) const {
    // This is optimized for `this.size() << other.size()`.
    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (it != end) {
      if (std::distance(it, end) > std::distance(other_it, other_end)) {
        return false;
      }
      other_it = std::lower_bound(other_it, other_end, *it, Compare());
      if (other_it == other_end || !Equal()(*it, *other_it)) {
        return false;
      }
      ++it;
      ++other_it;
    }
    return true;
  }

  bool equals(const FlatSet& other) const {
    return std::equal(m_vector.begin(), m_vector.end(), other.m_vector.begin(),
                      other.m_vector.end(), Equal());
  }

  friend bool operator==(const FlatSet& s1, const FlatSet& s2) {
    return s1.equals(s2);
  }

  friend bool operator!=(const FlatSet& s1, const FlatSet& s2) {
    return !s1.equals(s2);
  }

  FlatSet& insert(Element key) {
    auto it =
        std::lower_bound(m_vector.begin(), m_vector.end(), key, Compare());
    if (it == m_vector.end() || !Equal()(key, *it)) {
      m_vector.insert(it, std::move(key));
    }
    return *this;
  }

  FlatSet& remove(const Element& key) {
    auto it =
        std::lower_bound(m_vector.begin(), m_vector.end(), key, Compare());
    if (it != m_vector.end() && Equal()(key, *it)) {
      m_vector.erase(it);
    }
    return *this;
  }

  FlatSet& filter(const std::function<bool(const Element&)>& predicate) {
    m_vector.erase(
        std::remove_if(m_vector.begin(), m_vector.end(),
                       [&](const Element& e) { return !predicate(e); }),
        m_vector.end());
    return *this;
  }

  FlatSet& union_with(const FlatSet& other) {
    // This is optimized for `this.size() >> other.size()`.
    auto it = m_vector.begin();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, m_vector.end(), *other_it, Compare());
      if (it == m_vector.end() || !Equal()(*it, *other_it)) {
        it = m_vector.insert(it, *other_it);
      }
      ++it;
      ++other_it;
    }
    return *this;
  }

  FlatSet& intersection_with(const FlatSet& other) {
    // This is optimized for `this.size() << other.size()`.
    auto first = m_vector.begin(); // Where to write the next element to keep.
    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (it != end) {
      other_it = std::lower_bound(other_it, other_end, *it, Compare());
      if (other_it != other_end && Equal()(*it, *other_it)) {
        if (first != it) {
          *first = std::move(*it);
        }
        ++first;
        ++other_it;
      }
      ++it;
    }
    m_vector.erase(first, end);
    return *this;
  }

  FlatSet& difference_with(const FlatSet& other) {
    // This is optimized for `this.size() >> other.size()`.
    auto it = m_vector.begin();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, m_vector.end(), *other_it);
      if (it != m_vector.end() && Equal()(*it, *other_it)) {
        it = m_vector.erase(it);
        ++it;
      }
      ++other_it;
    }
    return *this;
  }

  FlatSet get_union_with(const FlatSet& other) const {
    if (m_vector.size() > other.m_vector.size()) {
      auto result = *this;
      result.union_with(other);
      return result;
    } else {
      auto result = other;
      result.union_with(*this);
      return result;
    }
  }

  FlatSet get_intersection_with(const FlatSet& other) const {
    if (m_vector.size() < other.m_vector.size()) {
      auto result = *this;
      result.intersection_with(other);
      return result;
    } else {
      auto result = other;
      result.intersection_with(*this);
      return result;
    }
  }

  FlatSet get_difference_with(const FlatSet& other) const {
    auto result = *this;
    result.difference_with(other);
    return result;
  }

  void clear() { m_vector.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const FlatSet<Element>& s) {
    o << "{";
    for (auto it = s.begin(), end = s.end(); it != end;) {
      o << pt_util::Dereference<Element>()(*it);
      ++it;
      if (it != end) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  std::vector<Element> m_vector;
};

} // namespace sparta
