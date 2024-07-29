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
#include <ostream>

#include <boost/container/flat_set.hpp>

#include <sparta/AbstractSet.h>
#include <sparta/PatriciaTreeUtil.h>
#include <sparta/PerfectForwardCapture.h>

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
          typename Equal = std::equal_to<Element>,
          typename AllocatorOrContainer =
              boost::container::new_allocator<Element>>
class FlatSet final
    : public AbstractSet<
          FlatSet<Element, Compare, Equal, AllocatorOrContainer>> {
 private:
  using BoostFlatSet =
      boost::container::flat_set<Element, Compare, AllocatorOrContainer>;

 public:
  // C++ container concept member types
  using iterator = typename BoostFlatSet::const_iterator;
  using const_iterator = iterator;
  using value_type = Element;
  using difference_type = typename BoostFlatSet::difference_type;
  using size_type = typename BoostFlatSet::size_type;
  using const_reference = typename BoostFlatSet::const_reference;
  using const_pointer = typename BoostFlatSet::const_pointer;

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

  bool empty() const { return m_set.empty(); }

  std::size_t size() const { return m_set.size(); }

  std::size_t max_size() const { return m_set.max_size(); }

  iterator begin() const { return m_set.begin(); }

  iterator end() const { return m_set.end(); }

  bool contains(const Element& key) const { return m_set.contains(key); }

  bool is_subset_of(const FlatSet& other) const {
    // This is optimized for `this.size() << other.size()`.
    auto it = m_set.begin(), end = m_set.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
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
    return std::equal(m_set.begin(), m_set.end(), other.m_set.begin(),
                      other.m_set.end(), Equal());
  }

  FlatSet& insert(Element key) {
    m_set.insert(key);
    return *this;
  }

  FlatSet& remove(const Element& key) {
    m_set.erase(key);
    return *this;
  }

  template <typename Visitor> // void (const Element&)
  void visit(Visitor&& visitor) const {
    for (const auto& element : m_set) {
      visitor(element);
    }
  }

  /*
   * If the set is a singleton, returns a pointer to the element.
   * Otherwise, returns nullptr.
   */
  const Element* singleton() const {
    if (m_set.size() == 1) {
      return &*m_set.begin();
    } else {
      return nullptr;
    }
  }

  template <typename Predicate> // bool(const Element&)
  FlatSet& filter(Predicate&& predicate) {
    auto container = m_set.extract_sequence();
    container.erase(
        std::remove_if(
            container.begin(), container.end(),
            [predicate = fwd_capture(std::forward<Predicate>(predicate))](
                const Element& e) mutable { return !predicate.get()(e); }),
        container.end());
    m_set.adopt_sequence(boost::container::ordered_unique_range,
                         std::move(container));
    return *this;
  }

  FlatSet& union_with(const FlatSet& other) {
    // This is optimized for `this.size() >> other.size()`.
    auto it = m_set.begin(), end = m_set.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, *other_it, Compare());
      if (it == end || !Equal()(*it, *other_it)) {
        it = m_set.insert(it, *other_it);
        end = m_set.end();
      }
      ++it;
      ++other_it;
    }
    return *this;
  }

  FlatSet& intersection_with(const FlatSet& other) {
    // This is optimized for `this.size() << other.size()`.
    auto container = m_set.extract_sequence();
    auto first = container.begin(); // Where to write the next element to keep.
    auto it = container.begin(), end = container.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
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
    container.erase(first, end);
    m_set.adopt_sequence(boost::container::ordered_unique_range,
                         std::move(container));
    return *this;
  }

  FlatSet& difference_with(const FlatSet& other) {
    // This is optimized for `this.size() >> other.size()`.
    auto it = m_set.begin(), end = m_set.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, *other_it);
      if (it != end && Equal()(*it, *other_it)) {
        it = m_set.erase(it);
        end = m_set.end();
      }
      ++other_it;
    }
    return *this;
  }

  FlatSet get_union_with(const FlatSet& other) const {
    if (m_set.size() > other.m_set.size()) {
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
    if (m_set.size() < other.m_set.size()) {
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

  void clear() { m_set.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const FlatSet<Element>& s) {
    o << "{";
    for (auto it = s.begin(), end = s.end(); it != end;) {
      o << pt_util::deref(*it);
      ++it;
      if (it != end) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  BoostFlatSet m_set;
};

} // namespace sparta
