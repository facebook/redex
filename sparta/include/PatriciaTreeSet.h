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
#include <iterator>
#include <ostream>
#include <stack>
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
class PatriciaTreeIterator;

template <typename IntegerType>
inline bool contains(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline bool is_subset_of(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree2);

template <typename IntegerType>
inline bool equals(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree2);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> insert_leaf(
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType>>& leaf,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree);

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> remove(
    IntegerType key,
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
  using iterator = pt_impl::PatriciaTreeIterator<Element>;
  using const_iterator = iterator;
  using value_type = Element;
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

  size_t size() const {
    size_t s = 0;
    std::for_each(begin(), end(), [&s](const auto&) { ++s; });
    return s;
  }

  size_t max_size() const { return m_core.max_size(); }

  iterator begin() const { return iterator(m_core.m_tree); }

  iterator end() const { return iterator(); }

  bool contains(Element key) const {
    if (empty()) {
      return false;
    }
    return pt_impl::contains<IntegerType>(Codec::encode(key), m_core.m_tree);
  }

  bool is_subset_of(const PatriciaTreeSet& other) const {
    return pt_impl::is_subset_of<IntegerType>(m_core.m_tree,
                                              other.m_core.m_tree);
  }

  bool equals(const PatriciaTreeSet& other) const {
    return pt_impl::equals<IntegerType>(m_core.m_tree, other.m_core.m_tree);
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
    m_core.m_tree =
        pt_impl::remove<IntegerType>(Codec::encode(key), m_core.m_tree);
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
boost::intrusive_ptr<PatriciaTreeBranch<IntegerType>> join(
    IntegerType prefix0,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree0,
    IntegerType prefix1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree1) {
  IntegerType m = get_branching_bit(prefix0, prefix1);
  if (is_zero_bit(prefix0, m)) {
    return PatriciaTreeBranch<IntegerType>::make(
        mask(prefix0, m), m, tree0, tree1);
  } else {
    return PatriciaTreeBranch<IntegerType>::make(
        mask(prefix0, m), m, tree1, tree0);
  }
}

// This function is used by remove() to prevent the creation of branch nodes
// with only one child.
template <typename IntegerType>
boost::intrusive_ptr<PatriciaTree<IntegerType>> make_branch(
    IntegerType prefix,
    IntegerType branching_bit,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& left_tree,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& right_tree) {
  if (left_tree == nullptr) {
    return right_tree;
  }
  if (right_tree == nullptr) {
    return left_tree;
  }
  return PatriciaTreeBranch<IntegerType>::make(
      prefix, branching_bit, left_tree, right_tree);
}

template <typename IntegerType>
inline bool contains(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
  if (tree == nullptr) {
    return false;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    return key == leaf->key();
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree);
  if (is_zero_bit(key, branch->branching_bit())) {
    return contains(key, branch->left_tree());
  } else {
    return contains(key, branch->right_tree());
  }
}

template <typename IntegerType>
inline bool is_subset_of(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the inclusion test to run in sublinear time
    // when comparing Patricia trees that share some structure.
    return true;
  }
  if (tree1 == nullptr) {
    return true;
  }
  if (tree2 == nullptr) {
    return false;
  }
  if (tree1->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree1);
    return contains(leaf->key(), tree2);
  }
  if (tree2->is_leaf()) {
    return false;
  }
  const auto& branch1 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree1);
  const auto& branch2 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree2);
  if (branch1->prefix() == branch2->prefix() &&
      branch1->branching_bit() == branch2->branching_bit()) {
    return is_subset_of(branch1->left_tree(), branch2->left_tree()) &&
           is_subset_of(branch1->right_tree(), branch2->right_tree());
  }
  if (branch1->branching_bit() > branch2->branching_bit() &&
      match_prefix(
          branch1->prefix(), branch2->prefix(), branch2->branching_bit())) {
    if (is_zero_bit(branch1->prefix(), branch2->branching_bit())) {
      return is_subset_of(branch1->left_tree(), branch2->left_tree()) &&
             is_subset_of(branch1->right_tree(), branch2->left_tree());
    } else {
      return is_subset_of(branch1->left_tree(), branch2->right_tree()) &&
             is_subset_of(branch1->right_tree(), branch2->right_tree());
    }
  }
  return false;
}

// A Patricia tree is a canonical representation of the set of keys it contains.
// Hence, set equality is equivalent to structural equality of Patricia trees.
template <typename IntegerType>
inline bool equals(
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the equality test to run in sublinear time
    // when comparing Patricia trees that share some structure.
    return true;
  }
  if (tree1 == nullptr) {
    return tree2 == nullptr;
  }
  if (tree2 == nullptr) {
    return false;
  }
  // Since the hash codes are readily available (they're computed when the trees
  // are constructed), we can use them to cut short the equality test.
  if (tree1->hash() != tree2->hash()) {
    return false;
  }
  if (tree1->is_leaf()) {
    if (tree2->is_branch()) {
      return false;
    }
    const auto& leaf1 =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree1);
    const auto& leaf2 =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree2);
    return leaf1->key() == leaf2->key();
  }
  if (tree2->is_leaf()) {
    return false;
  }
  const auto& branch1 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree1);
  const auto& branch2 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree2);
  return branch1->prefix() == branch2->prefix() &&
         branch1->branching_bit() == branch2->branching_bit() &&
         equals(branch1->left_tree(), branch2->left_tree()) &&
         equals(branch1->right_tree(), branch2->right_tree());
}

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
    return pt_impl::join<IntegerType>(
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
  return pt_impl::join<IntegerType>(
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
    return pt_impl::join<IntegerType>(
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
  return pt_impl::join<IntegerType>(
      leaf->key(), leaf, branch->prefix(), branch);
}

template <typename IntegerType>
inline boost::intrusive_ptr<PatriciaTree<IntegerType>> remove(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    if (key == leaf->key()) {
      return nullptr;
    }
    return leaf;
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(tree);
  if (match_prefix(key, branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(key, branch->branching_bit())) {
      auto new_left_tree = remove(key, branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return branch;
      }
      return make_branch<IntegerType>(branch->prefix(),
                                      branch->branching_bit(),
                                      new_left_tree,
                                      branch->right_tree());
    } else {
      auto new_right_tree = remove(key, branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return branch;
      }
      return make_branch<IntegerType>(branch->prefix(),
                                      branch->branching_bit(),
                                      branch->left_tree(),
                                      new_right_tree);
    }
  }
  return branch;
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
    return make_branch<IntegerType>(branch->prefix(),
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
  return pt_impl::join(p, s, q, t);
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
    return contains(leaf->key(), t) ? leaf : nullptr;
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
    return contains(leaf->key(), s) ? leaf : nullptr;
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
    return contains(leaf->key(), t) ? nullptr : leaf;
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
    return remove(leaf->key(), s);
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

// The iterator basically performs a post-order traversal of the tree, pausing
// at each leaf.
template <typename Element>
class PatriciaTreeIterator final {
  using Codec = pt_util::Codec<Element>;

 public:
  // C++ iterator concept member types
  using iterator_category = std::forward_iterator_tag;
  using value_type = Element;
  using difference_type = std::ptrdiff_t;
  using pointer = const Element*;
  using reference = const Element&;

  using IntegerType = typename Codec::IntegerType;

  PatriciaTreeIterator() {}

  explicit PatriciaTreeIterator(
      boost::intrusive_ptr<PatriciaTree<IntegerType>> tree)
      : m_root(std::move(tree)) {
    if (m_root == nullptr) {
      return;
    }
    go_to_next_leaf(m_root);
  }

  PatriciaTreeIterator& operator++() {
    // We disallow incrementing the end iterator.
    RUNTIME_CHECK(m_leaf != nullptr, undefined_operation());
    if (m_stack.empty()) {
      // This means that we were on the rightmost leaf. We've reached the end of
      // the iteration.
      m_leaf = nullptr;
      return *this;
    }
    // Otherwise, we pop out a branch from the stack and move to the leftmost
    // leaf in its right-hand subtree.
    auto branch = m_stack.top();
    m_stack.pop();
    go_to_next_leaf(branch->right_tree());
    return *this;
  }

  PatriciaTreeIterator operator++(int) {
    PatriciaTreeIterator retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(const PatriciaTreeIterator& other) const {
    // Note that there's no need to check the stack (it's just used to traverse
    // the tree).
    return m_leaf == other.m_leaf;
  }

  bool operator!=(const PatriciaTreeIterator& other) const {
    return !(*this == other);
  }

  reference operator*() const { return Codec::decode(m_leaf->data()); }

  pointer operator->() const { return &(**this); }

 private:
  // The argument is never null.
  void go_to_next_leaf(
      const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
    auto* t = tree.get();
    // We go to the leftmost leaf, storing the branches that we're traversing
    // on the stack. By definition of a Patricia tree, a branch node always
    // has two children, hence the leftmost leaf always exists.
    while (t->is_branch()) {
      auto branch = static_cast<PatriciaTreeBranch<IntegerType>*>(t);
      m_stack.push(branch);
      t = branch->left_tree().get();
      // A branch node always has two children.
      RUNTIME_CHECK(t != nullptr, internal_error());
    }
    m_leaf = static_cast<PatriciaTreeLeaf<IntegerType>*>(t);
  }

  // We are holding on to the root of the tree to ensure that all its nested
  // branches and leaves stay alive for as long as the iterator stays alive.
  boost::intrusive_ptr<PatriciaTree<IntegerType>> m_root;
  std::stack<PatriciaTreeBranch<IntegerType>*> m_stack;
  PatriciaTreeLeaf<IntegerType>* m_leaf{nullptr};
};

} // namespace pt_impl

} // namespace sparta
