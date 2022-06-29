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

#include "Exceptions.h"
#include "PatriciaTreeCore.h"
#include "PatriciaTreeUtil.h"

namespace sparta {

// Forward declarations.
namespace pt_impl {

using Empty = pt_core::EmptyValue;

template <typename IntegerType>
using PatriciaTree = pt_core::PatriciaTreeNode<IntegerType, Empty>;

template <typename IntegerType>
using PatriciaTreeLeaf = pt_core::PatriciaTreeLeaf<IntegerType, Empty>;

template <typename IntegerType>
using PatriciaTreeBranch = pt_core::PatriciaTreeBranch<IntegerType, Empty>;

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert_leaf(
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType>>& leaf,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> filter(
    const std::function<bool(IntegerType)>& predicate,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> merge(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> intersect(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> diff(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t);

} // namespace pt_impl

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
class PatriciaTreeSet final {
  using Core = pt_core::PatriciaTreeCore<Element, pt_impl::Empty>;
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

  friend bool operator==(const PatriciaTreeSet& s1, const PatriciaTreeSet& s2) {
    return s1.equals(s2);
  }

  friend bool operator!=(const PatriciaTreeSet& s1, const PatriciaTreeSet& s2) {
    return !s1.equals(s2);
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
    m_core.m_tree =
        pt_impl::insert<IntegerType>(Codec::encode(key), m_core.m_tree);
    return *this;
  }

  PatriciaTreeSet& remove(Element key) {
    m_core.remove(key);
    return *this;
  }

  PatriciaTreeSet& filter(
      const std::function<bool(const Element&)>& predicate) {
    auto encoded_predicate = [&predicate](IntegerType key) {
      return predicate(Codec::decode(key));
    };
    m_core.m_tree =
        pt_impl::filter<IntegerType>(encoded_predicate, m_core.m_tree);
    return *this;
  }

  PatriciaTreeSet& union_with(const PatriciaTreeSet& other) {
    m_core.m_tree =
        pt_impl::merge<IntegerType>(m_core.m_tree, other.m_core.m_tree);
    return *this;
  }

  PatriciaTreeSet& intersection_with(const PatriciaTreeSet& other) {
    m_core.m_tree =
        pt_impl::intersect<IntegerType>(m_core.m_tree, other.m_core.m_tree);
    return *this;
  }

  PatriciaTreeSet& difference_with(const PatriciaTreeSet& other) {
    m_core.m_tree =
        pt_impl::diff<IntegerType>(m_core.m_tree, other.m_core.m_tree);
    return *this;
  }

  PatriciaTreeSet get_union_with(const PatriciaTreeSet& other) const {
    auto result = *this;
    result.union_with(other);
    return result;
  }

  PatriciaTreeSet get_intersection_with(const PatriciaTreeSet& other) const {
    auto result = *this;
    result.intersection_with(other);
    return result;
  }

  PatriciaTreeSet get_difference_with(const PatriciaTreeSet& other) const {
    auto result = *this;
    result.difference_with(other);
    return result;
  }

  /*
   * The hash codes are computed incrementally when the Patricia trees are
   * constructed. Hence, this method has complexity O(1).
   */
  size_t hash() const { return m_core.hash(); }

  void clear() { m_core.clear(); }

  friend std::ostream& operator<<(std::ostream& o,
                                  const PatriciaTreeSet<Element>& s) {
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

namespace pt_impl {

using namespace pt_util;

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
  if (tree == nullptr) {
    return PatriciaTreeLeaf<IntegerType>::make(key, Empty{});
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    if (key == leaf->key()) {
      return leaf;
    }
    return pt_core::join<IntegerType, Empty>(
        key,
        PatriciaTreeLeaf<IntegerType>::make(key, Empty{}),
        leaf->key(),
        leaf);
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree);
  if (match_prefix(key, branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(key, branch->branching_bit())) {
      auto new_left_tree = insert(key, branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return branch;
      }
      return PatriciaTreeBranch<IntegerType>::make(branch->prefix(),
                                                   branch->branching_bit(),
                                                   new_left_tree,
                                                   branch->right_tree());
    } else {
      auto new_right_tree = insert(key, branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return branch;
      }
      return PatriciaTreeBranch<IntegerType>::make(branch->prefix(),
                                                   branch->branching_bit(),
                                                   branch->left_tree(),
                                                   new_right_tree);
    }
  }
  return pt_core::join<IntegerType, Empty>(
      key,
      PatriciaTreeLeaf<IntegerType>::make(key, Empty{}),
      branch->prefix(),
      branch);
}

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert_leaf(
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType>>& leaf,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
  if (tree == nullptr) {
    return leaf;
  }
  if (tree->is_leaf()) {
    const auto& tree_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    if (leaf->key() == tree_leaf->key()) {
      return tree_leaf;
    }
    return pt_core::join<IntegerType, Empty>(
        leaf->key(), leaf, tree_leaf->key(), tree_leaf);
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree);
  if (match_prefix(leaf->key(), branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(leaf->key(), branch->branching_bit())) {
      auto new_left_tree = insert_leaf(leaf, branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return branch;
      }
      return PatriciaTreeBranch<IntegerType>::make(branch->prefix(),
                                                   branch->branching_bit(),
                                                   new_left_tree,
                                                   branch->right_tree());
    } else {
      auto new_right_tree = insert_leaf(leaf, branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return branch;
      }
      return PatriciaTreeBranch<IntegerType>::make(branch->prefix(),
                                                   branch->branching_bit(),
                                                   branch->left_tree(),
                                                   new_right_tree);
    }
  }
  return pt_core::join<IntegerType, Empty>(
      leaf->key(), leaf, branch->prefix(), branch);
}

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> filter(
    const std::function<bool(IntegerType key)>& predicate,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    return predicate(leaf->key()) ? leaf : nullptr;
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree);
  auto new_left_tree = filter(predicate, branch->left_tree());
  auto new_right_tree = filter(predicate, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return branch;
  } else {
    return pt_core::make_branch(branch->prefix(),
                                branch->branching_bit(),
                                new_left_tree,
                                new_right_tree);
  }
}

// We keep the notations of the paper so as to make the implementation easier
// to follow.
template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> merge(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t) {
  if (s == t) {
    // This conditional is what allows the union operation to complete in
    // sublinear time when the operands share some structure.
    return s;
  }
  if (s == nullptr) {
    return t;
  }
  if (t == nullptr) {
    return s;
  }
  // We need to check whether t is a leaf before we do the same for s.
  // Otherwise, if s and t are both leaves, we would end up inserting s into t.
  // This would violate the assumptions required by `reference_equals()`.
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
    return insert_leaf(leaf, s);
  }
  if (s->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(s);
    return insert_leaf(leaf, t);
  }
  const auto& s_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(s);
  const auto& t_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(t);
  IntegerType m = s_branch->branching_bit();
  IntegerType n = t_branch->branching_bit();
  IntegerType p = s_branch->prefix();
  IntegerType q = t_branch->prefix();
  const auto& s0 = s_branch->left_tree();
  const auto& s1 = s_branch->right_tree();
  const auto& t0 = t_branch->left_tree();
  const auto& t1 = t_branch->right_tree();
  if (m == n && p == q) {
    // The two trees have the same prefix. We just merge the subtrees.
    auto new_left = merge(s0, t0);
    auto new_right = merge(s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    }
    if (new_left == t0 && new_right == t1) {
      return t;
    }
    return PatriciaTreeBranch<IntegerType>::make(p, m, new_left, new_right);
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Merge t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = merge(s0, t);
      if (s0 == new_left) {
        return s;
      }
      return PatriciaTreeBranch<IntegerType>::make(p, m, new_left, s1);
    } else {
      auto new_right = merge(s1, t);
      if (s1 == new_right) {
        return s;
      }
      return PatriciaTreeBranch<IntegerType>::make(p, m, s0, new_right);
    }
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Merge s with a subtree of t.
    if (is_zero_bit(p, n)) {
      auto new_left = merge(s, t0);
      if (t0 == new_left) {
        return t;
      }
      return PatriciaTreeBranch<IntegerType>::make(q, n, new_left, t1);
    } else {
      auto new_right = merge(s, t1);
      if (t1 == new_right) {
        return t;
      }
      return PatriciaTreeBranch<IntegerType>::make(q, n, t0, new_right);
    }
  }
  // The prefixes disagree.
  return pt_core::join(p, s, q, t);
}

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> intersect(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t) {
  if (s == t) {
    // This conditional is what allows the intersection operation to complete in
    // sublinear time when the operands share some structure.
    return s;
  }
  if (s == nullptr || t == nullptr) {
    return nullptr;
  }
  if (s->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(s);
    return pt_core::contains_key(leaf->key(), t) ? leaf : nullptr;
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
    return pt_core::contains_key(leaf->key(), s) ? leaf : nullptr;
  }
  const auto& s_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(s);
  const auto& t_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(t);
  IntegerType m = s_branch->branching_bit();
  IntegerType n = t_branch->branching_bit();
  IntegerType p = s_branch->prefix();
  IntegerType q = t_branch->prefix();
  const auto& s0 = s_branch->left_tree();
  const auto& s1 = s_branch->right_tree();
  const auto& t0 = t_branch->left_tree();
  const auto& t1 = t_branch->right_tree();
  if (m == n && p == q) {
    // The two trees have the same prefix. We merge the intersection of the
    // corresponding subtrees.
    return merge(intersect(s0, t0), intersect(s1, t1));
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Intersect t with a subtree of s.
    return intersect(is_zero_bit(q, m) ? s0 : s1, t);
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Intersect s with a subtree of t.
    return intersect(s, is_zero_bit(p, n) ? t0 : t1);
  }
  // The prefixes disagree.
  return nullptr;
}

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> diff(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& t) {
  if (s == t) {
    // This conditional is what allows the intersection operation to complete in
    // sublinear time when the operands share some structure.
    return nullptr;
  }
  if (s == nullptr) {
    return nullptr;
  }
  if (t == nullptr) {
    return s;
  }
  if (s->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(s);
    return pt_core::contains_key(leaf->key(), t) ? nullptr : leaf;
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
    return pt_core::remove(leaf->key(), s);
  }
  const auto& s_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(s);
  const auto& t_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(t);
  IntegerType m = s_branch->branching_bit();
  IntegerType n = t_branch->branching_bit();
  IntegerType p = s_branch->prefix();
  IntegerType q = t_branch->prefix();
  const auto& s0 = s_branch->left_tree();
  const auto& s1 = s_branch->right_tree();
  const auto& t0 = t_branch->left_tree();
  const auto& t1 = t_branch->right_tree();
  if (m == n && p == q) {
    // The two trees have the same prefix. We merge the difference of the
    // corresponding subtrees.
    return merge(diff(s0, t0), diff(s1, t1));
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Diff t with a subtree of s.
    if (is_zero_bit(q, m)) {
      return merge(diff(s0, t), s1);
    } else {
      return merge(s0, diff(s1, t));
    }
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Diff s with a subtree of t.
    if (is_zero_bit(p, n)) {
      return diff(s, t0);
    } else {
      return diff(s, t1);
    }
  }
  // The prefixes disagree.
  return s;
}

} // namespace pt_impl

} // namespace sparta
