/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <type_traits>
#include <utility>

#include <sparta/AbstractSet.h>
#include <sparta/Exceptions.h>
#include <sparta/PatriciaTreeCore.h>
#include <sparta/PatriciaTreeUtil.h>

namespace sparta {

/*
 * This implementation of sets of integers using Patricia trees is based on the
 * following paper:
 *
 *   C. Okasaki, A. Gill. Fast Mergeable Integer Maps. In Workshop on ML (1998).
 *
 * Patricia trees are a highly efficient representation of compressed binary
 * tries. They are well suited for the situation where one has to manipulate
 * many large sets that are identical or nearly identical. In the paper,
 * Patricia trees are entirely reconstructed for each operation. We have
 * modified the original algorithms, so that subtrees that are not affected by
 * an operation remain unchanged. Since this is a functional data structure,
 * identical subtrees are therefore shared among all Patricia tries manipulated
 * by the program. This effectively achieves a form of incremental hash-consing.
 * Note that it's not perfect, since identical trees that are independently
 * constructed are not equated, but it's a lot more efficient than regular
 * hash-consing. This data structure doesn't just reduce the memory footprint of
 * sets, it also significantly speeds up certain operations. Whenever two sets
 * represented as Patricia trees share some structure, their union and
 * intersection can often be computed in sublinear time.
 *
 * Patricia trees can only handle unsigned integers. Arbitrary objects can be
 * accommodated as long as they are represented as pointers. Our implementation
 * of Patricia-tree sets can transparently operate on either unsigned integers
 * or pointers to objects.
 */
template <typename Element>
class PatriciaTreeSet final : public AbstractSet<PatriciaTreeSet<Element>> {
  using Empty = pt_core::EmptyValue;
  using Core = pt_core::PatriciaTreeCore<Element, Empty>;
  using Codec = typename Core::Codec;

 public:
  // C++ container concept member types
  using value_type = Element;
  using iterator = typename Core::IteratorType;
  using const_iterator = iterator;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const Element&;
  using const_pointer = const Element*;

  using IntegerType = typename Codec::IntegerType;

  PatriciaTreeSet() = default;

  explicit PatriciaTreeSet(Element e) { insert(std::move(e)); }

  explicit PatriciaTreeSet(std::initializer_list<Element> l) {
    for (Element x : l) {
      insert(x);
    }
  }

  template <typename InputIterator>
  PatriciaTreeSet(InputIterator first, InputIterator last) {
    for (auto it = first; it != last; ++it) {
      insert(*it);
    }
  }

  bool empty() const { return m_core.empty(); }

  size_t size() const { return m_core.size(); }

  size_t max_size() const { return m_core.max_size(); }

  iterator begin() const { return m_core.begin(); }

  iterator end() const { return m_core.end(); }

  bool contains(Element key) const { return m_core.contains(key); }

  bool is_subset_of(const PatriciaTreeSet& other) const {
    return m_core.is_subset_of(other.m_core);
  }

  bool equals(const PatriciaTreeSet& other) const {
    return m_core.equals(other.m_core);
  }

  /*
   * This faster equality predicate can be used to check whether a sequence of
   * in-place modifications leaves a Patricia-tree set unchanged. For comparing
   * two arbitrary Patricia-tree sets, one needs to use the `equals()`
   * predicate.
   *
   * Example:
   *
   *   PatriciaTreeSet<...> s, t;
   *   ...
   *   t = s;
   *   t.union_with(...);
   *   t.remove(...);
   *   t.intersection_with(...);
   *   if (s.reference_equals(t)) { // This test is equivalent to s.equals(t)
   *     ...
   *   }
   */
  bool reference_equals(const PatriciaTreeSet& other) const {
    return m_core.reference_equals(other.m_core);
  }

  PatriciaTreeSet& insert(Element key) {
    m_core.upsert(key, Empty{});
    return *this;
  }

  PatriciaTreeSet& remove(Element key) {
    m_core.remove(key);
    return *this;
  }

  /*
   * If the set is a singleton, returns a pointer to the element.
   * Otherwise, returns nullptr.
   */
  const Element* singleton() const { return m_core.as_leaf_key(); }

  template <typename Predicate> // bool(const Element&)
  PatriciaTreeSet& filter(Predicate&& predicate) {
    m_core.filter([&](Element key, const Empty&) { return predicate(key); });
    return *this;
  }

  /*
   * Visit all elements.
   * This does NOT allocate memory, unlike the iterators.
   */
  template <typename Visitor> // void(const Element&)
  void visit(Visitor&& visitor) const {
    m_core.visit_all_leafs(std::forward<Visitor>(visitor));
  }

  bool erase_all_matching(Element element_mask) {
    return m_core.erase_all_matching(element_mask);
  }

  PatriciaTreeSet& union_with(const PatriciaTreeSet& other) {
    // For union, empty value or empty value is empty value.
    m_core.merge(pt_core::use_available_leaf<IntegerType, Empty>, other.m_core);
    return *this;
  }

  PatriciaTreeSet& intersection_with(const PatriciaTreeSet& other) {
    // For intersect, empty value and empty value is empty value.
    m_core.intersect(pt_core::use_available_leaf<IntegerType, Empty>,
                     other.m_core);
    return *this;
  }

  PatriciaTreeSet& difference_with(const PatriciaTreeSet& other) {
    // For diff, empty value without empty value is no value.
    m_core.diff([](const auto&...) { return nullptr; }, other.m_core);
    return *this;
  }

  /*
   * The hash codes are computed incrementally when the Patricia trees are
   * constructed. Hence, this method has complexity O(1).
   */
  size_t hash() const { return m_core.hash(); }

  void clear() { m_core.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const PatriciaTreeSet& s) {
    o << "{";
    for (auto it = s.begin(); it != s.end(); ++it) {
      o << pt_util::deref(*it);
      if (std::next(it) != s.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  Core m_core;
};

} // namespace sparta
