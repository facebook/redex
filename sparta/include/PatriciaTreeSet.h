/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ostream>
#include <stack>
#include <type_traits>
#include <utility>

#include <boost/functional/hash.hpp>
#include <boost/intrusive_ptr.hpp>

#include "Exceptions.h"
#include "PatriciaTreeUtil.h"

namespace sparta {

// Forward declarations.
namespace pt_impl {

template <typename IntegerType>
class PatriciaTree;

template <typename IntegerType>
class PatriciaTreeLeaf;

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
 public:
  // C++ container concept member types
  using iterator = pt_impl::PatriciaTreeIterator<Element>;
  using const_iterator = iterator;
  using value_type = Element;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const Element&;
  using const_pointer = const Element*;

  using IntegerType = typename std::
      conditional_t<std::is_pointer<Element>::value, uintptr_t, Element>;

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

  bool empty() const { return m_tree == nullptr; }

  size_t size() const {
    size_t s = 0;
    std::for_each(begin(), end(), [&s](const auto&) { ++s; });
    return s;
  }

  size_t max_size() const { return std::numeric_limits<IntegerType>::max(); }

  iterator begin() const { return iterator(m_tree); }

  iterator end() const { return iterator(); }

  bool contains(Element key) const {
    if (m_tree == nullptr) {
      return false;
    }
    return pt_impl::contains<IntegerType>(encode(key), m_tree);
  }

  bool is_subset_of(const PatriciaTreeSet& other) const {
    return pt_impl::is_subset_of<IntegerType>(m_tree, other.m_tree);
  }

  bool equals(const PatriciaTreeSet& other) const {
    return pt_impl::equals<IntegerType>(m_tree, other.m_tree);
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
    return m_tree == other.m_tree;
  }

  PatriciaTreeSet& insert(Element key) {
    m_tree = pt_impl::insert<IntegerType>(encode(key), m_tree);
    return *this;
  }

  PatriciaTreeSet& remove(Element key) {
    m_tree = pt_impl::remove<IntegerType>(encode(key), m_tree);
    return *this;
  }

  PatriciaTreeSet& filter(
      const std::function<bool(const Element&)>& predicate) {
    auto encoded_predicate = [&predicate](IntegerType key) {
      return predicate(decode(key));
    };
    m_tree = pt_impl::filter<IntegerType>(encoded_predicate, m_tree);
    return *this;
  }

  PatriciaTreeSet& union_with(const PatriciaTreeSet& other) {
    m_tree = pt_impl::merge<IntegerType>(m_tree, other.m_tree);
    return *this;
  }

  PatriciaTreeSet& intersection_with(const PatriciaTreeSet& other) {
    m_tree = pt_impl::intersect<IntegerType>(m_tree, other.m_tree);
    return *this;
  }

  PatriciaTreeSet& difference_with(const PatriciaTreeSet& other) {
    m_tree = pt_impl::diff<IntegerType>(m_tree, other.m_tree);
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
  size_t hash() const { return m_tree == nullptr ? 0 : m_tree->hash(); }

  void clear() { m_tree.reset(); }

  friend std::ostream& operator<<(std::ostream& o,
                                  const PatriciaTreeSet<Element>& s) {
    o << "{";
    for (auto it = s.begin(); it != s.end(); ++it) {
      o << pt_util::Dereference<Element>()(*it);
      if (std::next(it) != s.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  // These functions are used to handle the type conversions required when
  // manipulating sets of pointers. The first parameter is necessary to make
  // template deduction work.
  template <typename T = Element,
            typename std::enable_if_t<std::is_pointer<T>::value, int> = 0>
  static uintptr_t encode(Element x) {
    return reinterpret_cast<uintptr_t>(x);
  }

  template <typename T = Element,
            typename std::enable_if_t<!std::is_pointer<T>::value, int> = 0>
  static Element encode(Element x) {
    return x;
  }

  template <typename T = Element,
            typename std::enable_if_t<std::is_pointer<T>::value, int> = 0>
  static Element decode(uintptr_t x) {
    return reinterpret_cast<Element>(x);
  }

  template <typename T = Element,
            typename std::enable_if_t<!std::is_pointer<T>::value, int> = 0>
  static Element decode(Element x) {
    return x;
  }

  boost::intrusive_ptr<pt_impl::PatriciaTree<IntegerType>> m_tree;

  template <typename T>
  friend class pt_impl::PatriciaTreeIterator;
};

namespace pt_impl {

using namespace pt_util;

template <typename IntegerType>
class PatriciaTree {
 public:
  // A Patricia tree is an immutable structure.
  PatriciaTree& operator=(const PatriciaTree& other) = delete;

  virtual ~PatriciaTree() {
    // The destructor is the only method that is guaranteed to be created when
    // a class template is instantiated. This is a good place to perform all
    // the sanity checks on the template parameters.
    static_assert(std::is_unsigned<IntegerType>::value,
                  "IntegerType is not an unsigned arihmetic type");
  }

  virtual bool is_leaf() const = 0;

  bool is_branch() const { return !is_leaf(); }

  size_t hash() const { return m_hash; }

  void set_hash(size_t h) { m_hash = h; }

  friend void intrusive_ptr_add_ref(const PatriciaTree<IntegerType>* p) {
    p->m_reference_count.fetch_add(1, std::memory_order_relaxed);
  }

  friend void intrusive_ptr_release(const PatriciaTree<IntegerType>* p) {
    if (p->m_reference_count.fetch_sub(1, std::memory_order_release) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      delete p;
    }
  }

 private:
  mutable std::atomic<size_t> m_reference_count{0};
  size_t m_hash;
};

// This defines an internal node of a Patricia tree. Patricia trees are
// compressed binary tries, where a path in the tree represents a sequence of
// branchings based on the value of some bits at certain positions in the binary
// decomposition of the key. The position of the bit in the key which determines
// the branching at a given node is represented in m_branching_bit as a bit mask
// (i.e., all bits are 0 except for the branching bit). All keys in the subtree
// originating from a given node share the same bit prefix (in the little endian
// ordering), which is stored in m_prefix.
template <typename IntegerType>
class PatriciaTreeBranch final : public PatriciaTree<IntegerType> {
 public:
  PatriciaTreeBranch(IntegerType prefix,
                     IntegerType branching_bit,
                     boost::intrusive_ptr<PatriciaTree<IntegerType>> left_tree,
                     boost::intrusive_ptr<PatriciaTree<IntegerType>> right_tree)
      : m_prefix(prefix),
        m_branching_bit(branching_bit),
        m_left_tree(std::move(left_tree)),
        m_right_tree(std::move(right_tree)) {
    size_t seed = 0;
    boost::hash_combine(seed, m_prefix);
    boost::hash_combine(seed, m_branching_bit);
    boost::hash_combine(seed, m_left_tree->hash());
    boost::hash_combine(seed, m_right_tree->hash());
    this->set_hash(seed);
  }

  bool is_leaf() const override { return false; }

  IntegerType prefix() const { return m_prefix; }

  IntegerType branching_bit() const { return m_branching_bit; }

  const boost::intrusive_ptr<PatriciaTree<IntegerType>>& left_tree() const {
    return m_left_tree;
  }

  const boost::intrusive_ptr<PatriciaTree<IntegerType>>& right_tree() const {
    return m_right_tree;
  }

  static boost::intrusive_ptr<PatriciaTreeBranch<IntegerType>> make(
      IntegerType prefix,
      IntegerType branching_bit,
      boost::intrusive_ptr<PatriciaTree<IntegerType>> left_tree,
      boost::intrusive_ptr<PatriciaTree<IntegerType>> right_tree) {
    return new PatriciaTreeBranch<IntegerType>(
        prefix, branching_bit, std::move(left_tree), std::move(right_tree));
  }

 private:
  IntegerType m_prefix;
  IntegerType m_branching_bit;
  boost::intrusive_ptr<PatriciaTree<IntegerType>> m_left_tree;
  boost::intrusive_ptr<PatriciaTree<IntegerType>> m_right_tree;
};

template <typename IntegerType>
class PatriciaTreeLeaf final : public PatriciaTree<IntegerType> {
 public:
  explicit PatriciaTreeLeaf(IntegerType key) : m_key(key) {
    boost::hash<IntegerType> hasher;
    this->set_hash(hasher(key));
  }

  bool is_leaf() const override { return true; }

  const IntegerType& key() const { return m_key; }

  static boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType>> make(
      IntegerType key) {
    return new PatriciaTreeLeaf<IntegerType>(key);
  }

 private:
  IntegerType m_key;
};

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
    return PatriciaTreeLeaf<IntegerType>::make(key);
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(tree);
    if (key == leaf->key()) {
      return leaf;
    }
    return pt_impl::join<IntegerType>(
        key, PatriciaTreeLeaf<IntegerType>::make(key), leaf->key(), leaf);
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
      key, PatriciaTreeLeaf<IntegerType>::make(key), branch->prefix(), branch);
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
 public:
  // C++ iterator concept member types
  using iterator_category = std::forward_iterator_tag;
  using value_type = Element;
  using difference_type = std::ptrdiff_t;
  using pointer = Element*;
  using reference = const Element&;

  using IntegerType = typename PatriciaTreeSet<Element>::IntegerType;

  PatriciaTreeIterator() {}

  explicit PatriciaTreeIterator(
      const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
    if (tree == nullptr) {
      return;
    }
    go_to_next_leaf(tree);
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

  Element operator*() {
    return PatriciaTreeSet<Element>::decode(m_leaf->key());
  }

 private:
  // The argument is never null.
  void go_to_next_leaf(
      const boost::intrusive_ptr<PatriciaTree<IntegerType>>& tree) {
    auto t = tree;
    // We go to the leftmost leaf, storing the branches that we're traversing
    // on the stack. By definition of a Patricia tree, a branch node always
    // has two children, hence the leftmost leaf always exists.
    while (t->is_branch()) {
      auto branch =
          boost::static_pointer_cast<PatriciaTreeBranch<IntegerType>>(t);
      m_stack.push(branch);
      t = branch->left_tree();
      // A branch node always has two children.
      RUNTIME_CHECK(t != nullptr, internal_error());
    }
    m_leaf = boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType>>(t);
  }

  std::stack<boost::intrusive_ptr<PatriciaTreeBranch<IntegerType>>> m_stack;
  boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType>> m_leaf;
};

} // namespace pt_impl

} // namespace sparta
