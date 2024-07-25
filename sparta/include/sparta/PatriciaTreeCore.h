/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stack>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/functional/hash.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

#include <sparta/AbstractDomain.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/Exceptions.h>
#include <sparta/PatriciaTreeUtil.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {

namespace pt_core {

/*
 * This structure implements a map of integer/pointer keys to (possibly empty)
 * values, optionally maintaining hashes. It's based on the following paper:
 *
 *   C. Okasaki, A. Gill. Fast Mergeable Integer Maps. In Workshop on ML (1998).
 *
 * This is the core structure common to both maps and sets. In typical fashion
 * a map is the core representation, with a set being a map to an empty value.
 * However, a set provides a hash at each node which a map does not do - so
 * similarly a map will provide an empty value for the hash.
 *
 * To allow empty hashes and values without introducing storage overhead, we
 * use specialization to factor out the storage differences. To avoid excessive
 * monomorphization overhead, we prefer to define over IntegerType and Value so
 * that each distinct pointer Key may reuse the same underlying implementation.
 *
 * (An alternative is to use empty base optimization via std::tuple, to do
 * this implicitly. However, the public interface for map already assumes we
 * use std::pair, and std::tuple isn't guaranteed to use EBO anyway.)
 */
template <typename Key, typename Value>
class PatriciaTreeCore;

template <typename Key, typename Value>
class PatriciaTreeIterator;

/*
 * Convenience interface that makes it easy to define maps for value types that
 * are default-constructible and equality-comparable.
 */
template <typename T>
struct SimpleValue final : public AbstractMapValue<SimpleValue<T>> {
  using type = T;

  static T default_value() { return T(); }

  static bool is_default_value(const T& t) { return t == T(); }

  static bool equals(const T& a, const T& b) { return a == b; }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Value;
};

/*
 * The empty map value on which we specialize.
 */
struct EmptyValue {
  // We do need to have something to construct and pass around,
  // even if ultimately it never manifests in the final binary.
  using type = EmptyValue;

  static type default_value() { return type(); }

  static bool is_default_value(const type&) { return true; }

  static bool equals(const type&, const type&) { return true; }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Value;
};

template <typename IntegerType, typename Value>
class PatriciaTreeLeaf;

template <typename IntegerType, typename Value>
class PatriciaTreeBranch;

/*
 * Base node common to branches and leafs.
 */
template <typename IntegerType, typename Value>
class PatriciaTreeNode {
 public:
  using LeafType = PatriciaTreeLeaf<IntegerType, Value>;
  using BranchType = PatriciaTreeBranch<IntegerType, Value>;

  static_assert(std::is_unsigned_v<IntegerType>,
                "IntegerType is not an unsigned arithmetic type");

  // A Patricia tree is an immutable structure.
  PatriciaTreeNode(const PatriciaTreeNode&) = delete;
  PatriciaTreeNode& operator=(const PatriciaTreeNode& other) = delete;

  bool is_leaf() const {
    return m_reference_count.load(std::memory_order_relaxed) & LEAF_MASK;
  }

  bool is_branch() const { return !is_leaf(); }

  // Returns nullptr if this node is not a leaf.
  const LeafType* as_leaf() const {
    return is_leaf() ? static_cast<const LeafType*>(this) : nullptr;
  }

  // Returns nullptr if this node is not a branch.
  const BranchType* as_branch() const {
    return is_branch() ? static_cast<const BranchType*>(this) : nullptr;
  }

  size_t hash() const {
    if (const auto* leaf = as_leaf()) {
      return leaf->hash();
    } else {
      // At the time of writing, the pattern of checking for leaf/branch and
      // then falling back to branch/leaf using as_{leaf,branch} emits exactly
      // one redundant mov instruction.
      //
      // Although C++ allows a relaxed atomic read to fold into the previous
      // one, no compiler currently does so. Clang issues another read to the
      // refcount, then immediately overwrites the destination register. The
      // result is then the same as if we statically casted and dereferenced.
      //
      // The convenience of this pattern outweighs the extra instruction, but
      // I wanted to call it out. Register renaming should make it ~zero cost.
      return as_branch()->hash();
    }
  }

 protected:
  // The reference count begins at 1, accounting for the caller.
  explicit PatriciaTreeNode(bool is_leaf)
      : m_reference_count((is_leaf ? LEAF_MASK : 0) + 1) {}

 private:
  friend void intrusive_ptr_add_ref(const PatriciaTreeNode* p) {
    p->m_reference_count.fetch_add(1, std::memory_order_relaxed);
  }

  static void intrusive_ptr_delete_leaf(const PatriciaTreeNode* p) {
    delete static_cast<const LeafType*>(p);
  }

  static void intrusive_ptr_delete_branch(const PatriciaTreeNode* p) {
    delete static_cast<const BranchType*>(p);
  }

  static void intrusive_ptr_delete(const PatriciaTreeNode* p) {
    // This reloads what was already known by the caller, but it helps put
    // more instructions behind a function call. Since we just poked the
    // reference count this is effectively zero cost.
    //
    // In C++20 we could use std::atomic_ref to make this non-atomic,
    // but on x86 its already just a normal load anyway.
    size_t reference_count =
        p->m_reference_count.load(std::memory_order_relaxed);
    const bool is_leaf = reference_count & LEAF_MASK;

    std::atomic_thread_fence(std::memory_order_acquire);
    if (is_leaf) {
      intrusive_ptr_delete_leaf(p);
    } else {
      intrusive_ptr_delete_branch(p);
    }
  }

  friend void intrusive_ptr_release(const PatriciaTreeNode* p) {
    size_t prev_reference_count =
        p->m_reference_count.fetch_sub(1, std::memory_order_release);
    const bool is_unique = (prev_reference_count & ~LEAF_MASK) == 1;
    if (is_unique) {
      intrusive_ptr_delete(p);
    }
  }

  // We are stealing the highest bit of our reference counter to indicate
  // whether this tree is a leaf (or, otherwise, branch).
  static constexpr size_t LEAF_MASK = ~(static_cast<size_t>(-1) >> 1);
  mutable std::atomic<size_t> m_reference_count;
};

/*
 * The base of leaf nodes optionally storing a value.
 */
template <typename IntegerType, typename Value>
class PatriciaTreeLeafBase {
 public:
  using ValueType = typename Value::type;
  using StorageType = std::pair<IntegerType, ValueType>;

  const StorageType& data() const { return m_pair; }

  const IntegerType& key() const { return m_pair.first; }

  const ValueType& value() const { return m_pair.second; }

  constexpr size_t hash() const { return 0; }

 protected:
  PatriciaTreeLeafBase(IntegerType key, ValueType value)
      : m_pair(key, std::move(value)) {}

 private:
  StorageType m_pair;
};

template <typename IntegerType>
class PatriciaTreeLeafBase<IntegerType, EmptyValue> : private EmptyValue {
 public:
  using ValueType = EmptyValue;
  using StorageType = IntegerType;

  const StorageType& data() const { return m_key; }

  const IntegerType& key() const { return m_key; }

  const ValueType& value() const { return *this; }

  size_t hash() const { return boost::hash<IntegerType>{}(m_key); }

 protected:
  PatriciaTreeLeafBase(IntegerType key, ValueType) : m_key(key) {}

 private:
  StorageType m_key;
};

/*
 * A leaf node, optionally storing a value.
 */
template <typename IntegerType, typename Value>
class PatriciaTreeLeaf final : public PatriciaTreeNode<IntegerType, Value>,
                               public PatriciaTreeLeafBase<IntegerType, Value> {
  using Base = PatriciaTreeNode<IntegerType, Value>;
  using LeafBase = PatriciaTreeLeafBase<IntegerType, Value>;

 public:
  using ValueType = typename LeafBase::ValueType;

  explicit PatriciaTreeLeaf(IntegerType key, ValueType value)
      : Base(/* is_leaf */ true), LeafBase(key, std::move(value)) {}

  using LeafBase::hash;

  static inline boost::intrusive_ptr<PatriciaTreeLeaf> make(IntegerType key,
                                                            ValueType value) {
    return boost::intrusive_ptr<PatriciaTreeLeaf>(
        new PatriciaTreeLeaf(key, std::move(value)), /* add_ref */ false);
  }
};

/*
 * The base of branch nodes optionally storing a hash.
 */
template <bool HasHash>
class PatriciaTreeBranchBase;

template <>
class PatriciaTreeBranchBase</* HashHash */ true> {
 public:
  size_t hash() const { return m_hash; }

 protected:
  template <typename IntegerType, typename Value>
  PatriciaTreeBranchBase(
      IntegerType prefix,
      IntegerType branching_bit,
      const PatriciaTreeNode<IntegerType, Value>& left_tree,
      const PatriciaTreeNode<IntegerType, Value>& right_tree) {
    boost::hash_combine(m_hash, prefix);
    boost::hash_combine(m_hash, branching_bit);
    boost::hash_combine(m_hash, left_tree.hash());
    boost::hash_combine(m_hash, right_tree.hash());
  }

 private:
  size_t m_hash{0};
};

template <>
class PatriciaTreeBranchBase</* HasHash */ false> {
 public:
  constexpr size_t hash() const { return 0; }

 protected:
  template <typename IntegerType, typename Value>
  PatriciaTreeBranchBase(IntegerType,
                         IntegerType,
                         const PatriciaTreeNode<IntegerType, Value>&,
                         const PatriciaTreeNode<IntegerType, Value>&) {}
};

/*
 * A branch node, optionally storing a hash.
 */
template <typename IntegerType, typename Value>
class PatriciaTreeBranch final
    : public PatriciaTreeNode<IntegerType, Value>,
      public PatriciaTreeBranchBase<std::is_same_v<Value, EmptyValue>> {
  using Base = PatriciaTreeNode<IntegerType, Value>;
  using BranchBase = PatriciaTreeBranchBase<std::is_same_v<Value, EmptyValue>>;

 public:
  PatriciaTreeBranch(IntegerType prefix,
                     IntegerType branching_bit,
                     boost::intrusive_ptr<Base> left_tree,
                     boost::intrusive_ptr<Base> right_tree)
      : Base(/* is_leaf */ false),
        BranchBase(prefix, branching_bit, *left_tree, *right_tree),
        m_prefix(prefix),
        m_branching_bit(branching_bit),
        m_left_tree(std::move(left_tree)),
        m_right_tree(std::move(right_tree)) {}

  using BranchBase::hash;

  IntegerType prefix() const { return m_prefix; }

  IntegerType branching_bit() const { return m_branching_bit; }

  const boost::intrusive_ptr<Base>& left_tree() const { return m_left_tree; }

  const boost::intrusive_ptr<Base>& right_tree() const { return m_right_tree; }

  static inline boost::intrusive_ptr<PatriciaTreeBranch> make(
      IntegerType prefix,
      IntegerType branching_bit,
      boost::intrusive_ptr<Base> left_tree,
      boost::intrusive_ptr<Base> right_tree) {
    return boost::intrusive_ptr<PatriciaTreeBranch>(
        new PatriciaTreeBranch(
            prefix, branching_bit, std::move(left_tree), std::move(right_tree)),
        /* add_ref */ false);
  }

 private:
  IntegerType m_prefix, m_branching_bit;
  boost::intrusive_ptr<Base> m_left_tree, m_right_tree;
};

// Advances over each leaf in the tree in post-order.
//
// This is the central core that iterators use to iterate,
// but without all the C++ iterator interface noise.
template <typename IntegerType, typename Value>
class PatriciaTreePostOrder final {
 public:
  using NodeType = PatriciaTreeNode<IntegerType, Value>;
  using LeafType = typename NodeType::LeafType;
  using BranchType = typename NodeType::BranchType;

  // Depending on the specialization of leaf node base, either a
  // single (encoded) key type, or a pair of (encoded) key and value.
  using StorageType = typename LeafType::StorageType;

  void advance() {
    // We disallow incrementing beyond the end.
    SPARTA_RUNTIME_CHECK(m_leaf != nullptr, undefined_operation());

    if (m_stack.empty()) {
      // This means that we were on the rightmost leaf. We've reached the end
      // of the iteration.
      m_leaf = nullptr;
      return;
    }

    // Otherwise, we pop out a branch from the stack and move to the leftmost
    // leaf in its right-hand subtree.
    auto branch = m_stack.top();
    m_stack.pop();
    go_to_next_leaf(branch->right_tree());
  }

  bool equals(const PatriciaTreePostOrder& other) const {
    // Note that there's no need to check the stack (it's just used to
    // traverse the tree).
    return m_leaf == other.m_leaf;
  }

  const StorageType& data() const { return m_leaf->data(); }

 private:
  PatriciaTreePostOrder() = default;

  explicit PatriciaTreePostOrder(boost::intrusive_ptr<NodeType> tree)
      : m_root(std::move(tree)) {
    if (m_root != nullptr) {
      go_to_next_leaf(m_root);
    }
  }

  // The argument is never null.
  void go_to_next_leaf(const boost::intrusive_ptr<NodeType>& tree) {
    const NodeType* t = tree.get();

    // We go to the leftmost leaf, storing the branches that we're traversing
    // on the stack. By definition of a Patricia tree, a branch node always
    // has two children, hence the leftmost leaf always exists.
    while (const auto* branch = t->as_branch()) {
      m_stack.push(branch);

      t = branch->left_tree().get();
      // A branch node always has two children.
      SPARTA_RUNTIME_CHECK(t != nullptr, internal_error());
    }

    m_leaf = t->as_leaf();
  }

  // We are holding on to the root of the tree to ensure that all its nested
  // branches and leaves stay alive for as long as the iterator stays alive.
  boost::intrusive_ptr<NodeType> m_root;
  std::stack<const BranchType*> m_stack;
  const LeafType* m_leaf{nullptr};

  template <typename K, typename V>
  friend class PatriciaTreeIterator;
};

// The iterator performs a post-order traversal of the tree,
// pausing at each leaf.
template <typename Key, typename Value>
class PatriciaTreeIterator final {
  using Codec = pt_util::Codec<Key>;
  using IntegerType = typename Codec::IntegerType;

  using IteratorImpl = PatriciaTreePostOrder<IntegerType, Value>;
  using NodeType = typename IteratorImpl::NodeType;
  using StorageType = typename IteratorImpl::StorageType;
  using DecodedType =
      decltype(Codec::decode(std::declval<const StorageType&>()));

 public:
  // C++ iterator concept member types
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::decay_t<DecodedType>;
  using mapped_type = typename IteratorImpl::LeafType::ValueType;
  using difference_type = std::ptrdiff_t;
  using pointer = const value_type*;
  using reference = const value_type&;

  PatriciaTreeIterator() = default;

  PatriciaTreeIterator& operator++() {
    m_impl.advance();
    return *this;
  }

  PatriciaTreeIterator operator++(int) {
    auto retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(const PatriciaTreeIterator& other) const {
    return m_impl.equals(other.m_impl);
  }

  bool operator!=(const PatriciaTreeIterator& other) const {
    return !(*this == other);
  }

  reference operator*() const { return Codec::decode(m_impl.data()); }

  pointer operator->() const { return &(**this); }

 private:
  explicit PatriciaTreeIterator(boost::intrusive_ptr<NodeType> tree)
      : m_impl(std::move(tree)) {}

  IteratorImpl m_impl;

  template <typename K, typename V>
  friend class PatriciaTreeCore;
};

// Unfortunately we must explicitly state the type to allow for template
// deduction, rather than a short template `using`. Might as well shorten.
using boost::intrusive_ptr;

using pt_util::get_branching_bit;
using pt_util::get_lowest_bit;
using pt_util::is_zero_bit;
using pt_util::mask;
using pt_util::match_prefix;

// Returns a pointer to the leaf if present, else nullptr.
template <typename IntegerType, typename Value>
inline const PatriciaTreeLeaf<IntegerType, Value>* find_leaf_by_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (const auto* leaf = tree->as_leaf()) {
    if (key == leaf->key()) {
      return leaf;
    } else {
      return nullptr;
    }
  }
  const auto* branch = tree->as_branch();
  if (is_zero_bit(key, branch->branching_bit())) {
    return find_leaf_by_key(key, branch->left_tree());
  } else {
    return find_leaf_by_key(key, branch->right_tree());
  }
}

// Returns a new copy of the leaf's intrusive_ptr if present, else nullptr.
template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>>
clone_leaf_pointer_by_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  using LeafType = PatriciaTreeLeaf<IntegerType, Value>;
  const LeafType* leaf = find_leaf_by_key(key, tree);
  return intrusive_ptr<LeafType>(const_cast<LeafType*>(leaf));
}

// Returns a pointer to the leaf's value if present, else nullptr.
template <typename IntegerType, typename Value>
inline const typename Value::type* find_value_by_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  const auto* leaf = find_leaf_by_key(key, tree);
  return leaf ? &leaf->value() : nullptr;
}

template <typename IntegerType, typename Value>
inline bool contains_leaf_with_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  return find_leaf_by_key(key, tree) != nullptr;
}

template <typename IntegerType, typename Value>
inline bool is_tree_subset_of(
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree1,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the inclusion test to run in sublinear time
    // when comparing Patricia trees that share some structure.
    return true;
  } else if (tree1 == nullptr) {
    return true;
  } else if (tree2 == nullptr) {
    return false;
  }

  if (const auto* leaf = tree1->as_leaf()) {
    return contains_leaf_with_key(leaf->key(), tree2);
  } else if (tree2->is_leaf()) {
    return false;
  }

  const auto* branch1 = tree1->as_branch();
  const auto* branch2 = tree2->as_branch();
  if (branch1->prefix() == branch2->prefix() &&
      branch1->branching_bit() == branch2->branching_bit()) {
    return is_tree_subset_of(branch1->left_tree(), branch2->left_tree()) &&
           is_tree_subset_of(branch1->right_tree(), branch2->right_tree());
  } else if (branch1->branching_bit() > branch2->branching_bit() &&
             match_prefix(branch1->prefix(),
                          branch2->prefix(),
                          branch2->branching_bit())) {
    if (is_zero_bit(branch1->prefix(), branch2->branching_bit())) {
      return is_tree_subset_of(branch1->left_tree(), branch2->left_tree()) &&
             is_tree_subset_of(branch1->right_tree(), branch2->left_tree());
    } else {
      return is_tree_subset_of(branch1->left_tree(), branch2->right_tree()) &&
             is_tree_subset_of(branch1->right_tree(), branch2->right_tree());
    }
  }

  return false;
}

/* Assumes Value::default_value_kind is either Top or Bottom */
template <typename IntegerType, typename Value, typename Compare>
inline bool is_tree_leq(
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& s,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& t,
    Compare&& compare) {
  static_assert(Value::default_value_kind == AbstractValueKind::Top ||
                Value::default_value_kind == AbstractValueKind::Bottom);

  if (s == t) {
    // This condition allows the leq operation to run in sublinear time when
    // comparing Patricia trees that share some structure.
    return true;
  } else if (s == nullptr) {
    return Value::default_value_kind == AbstractValueKind::Bottom;
  } else if (t == nullptr) {
    return Value::default_value_kind == AbstractValueKind::Top;
  }

  const auto* s_leaf = s->as_leaf();
  const auto* t_leaf = t->as_leaf();
  if (s_leaf && t_leaf) {
    // Both nodes are leaves. s leq to t iff
    // key(s) == key(t) && value(s) <= value(t).
    return s_leaf->key() == t_leaf->key() &&
           compare(s_leaf->key(), s_leaf->value(), t_leaf->value());
  } else if (s_leaf) {
    // t has at least one non-default binding that s doesn't have.
    if (Value::default_value_kind == AbstractValueKind::Top) {
      // The non-default binding in t can never be <= Top.
      return false;
    }

    // Otherwise, find if t contains s.
    // The missing bindings in s are bound to Bottom in this case. Even if we
    // know t contains strictly more bindings than s, they all satisfy the leq
    // condition. For each key k in t but not in s, s[k] == Bottom <= t[k]
    // always hold.
    auto* t_value = find_value_by_key(s_leaf->key(), t);
    if (t_value == nullptr) {
      // Always false if default_value is Bottom, which we already assume.
      return false;
    } else {
      return compare(s_leaf->key(), s_leaf->value(), *t_value);
    }
  } else if (t_leaf) {
    // s has at least one non-default binding that t doesn't have.
    if (Value::default_value_kind == AbstractValueKind::Bottom) {
      // There exists a key such that s[key] != Bottom and t[key] == Bottom.
      return false;
    }

    auto* s_value = find_value_by_key(t_leaf->key(), s);
    if (s_value == nullptr) {
      // Always false if default_value is Top, which we already assume.
      return false;
    } else {
      return compare(t_leaf->key(), *s_value, t_leaf->value());
    }
  }

  // Neither s nor t is a leaf.
  const auto* s_branch = s->as_branch();
  const auto* t_branch = t->as_branch();
  IntegerType m = s_branch->branching_bit();
  IntegerType n = t_branch->branching_bit();
  IntegerType p = s_branch->prefix();
  IntegerType q = t_branch->prefix();
  const auto& s0 = s_branch->left_tree();
  const auto& s1 = s_branch->right_tree();
  const auto& t0 = t_branch->left_tree();
  const auto& t1 = t_branch->right_tree();
  if (m == n && p == q) {
    return is_tree_leq(s0, t0, compare) && is_tree_leq(s1, t1, compare);
  } else if (m < n && match_prefix(q, p, m)) {
    // The tree t only contains bindings present in a subtree of s, and s has
    // bindings not present in t.
    return Value::default_value_kind == AbstractValueKind::Top &&
           is_tree_leq(is_zero_bit(q, m) ? s0 : s1, t, compare);
  } else if (m > n && match_prefix(p, q, n)) {
    // The tree s only contains bindings present in a subtree of t, and t has
    // bindings not present in s.
    return Value::default_value_kind == AbstractValueKind::Bottom &&
           is_tree_leq(s, is_zero_bit(p, n) ? t0 : t1, compare);
  } else {
    // s and t both have bindings that are not present in the other tree.
    return false;
  }
}

template <typename IntegerType, typename Value>
inline bool is_tree_equal(
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree1,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the equality test to run in sublinear time
    // when comparing Patricia trees that share some structure.
    return true;
  } else if (tree1 == nullptr || tree2 == nullptr) {
    return false;
  }
  const auto* leaf1 = tree1->as_leaf();
  const auto* leaf2 = tree2->as_leaf();
  if (leaf1 && leaf2) {
    return leaf1->key() == leaf2->key() &&
           Value::equals(leaf1->value(), leaf2->value());
  } else if (leaf1 || leaf2) {
    return false;
  }

  const auto* branch1 = tree1->as_branch();
  const auto* branch2 = tree2->as_branch();
  return branch1->hash() == branch2->hash() &&
         branch1->prefix() == branch2->prefix() &&
         branch1->branching_bit() == branch2->branching_bit() &&
         is_tree_equal(branch1->left_tree(), branch2->left_tree()) &&
         is_tree_equal(branch1->right_tree(), branch2->right_tree());
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeBranch<IntegerType, Value>> join_trees(
    IntegerType prefix0,
    intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> tree0,
    IntegerType prefix1,
    intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> tree1) {
  IntegerType m = get_branching_bit(prefix0, prefix1);
  if (is_zero_bit(prefix0, m)) {
    return PatriciaTreeBranch<IntegerType, Value>::make(
        mask(prefix0, m), m, std::move(tree0), std::move(tree1));
  } else {
    return PatriciaTreeBranch<IntegerType, Value>::make(
        mask(prefix0, m), m, std::move(tree1), std::move(tree0));
  }
}

// This function is used to prevent the creation of branch nodes with only one
// child. Returns a subtree if one of left or right is null, else a new branch.
template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> make_branch(
    IntegerType prefix,
    IntegerType branching_bit,
    intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> left_tree,
    intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> right_tree) {
  if (left_tree == nullptr) {
    return right_tree;
  } else if (right_tree == nullptr) {
    return left_tree;
  } else {
    return PatriciaTreeBranch<IntegerType, Value>::make(
        prefix, branching_bit, std::move(left_tree), std::move(right_tree));
  }
}

/*
 * All of the callback functions below, like update/combine/map/etc, will
 * operate directly on leaf node intrusive pointer arguments. This allows
 * e.g. one to copy an intrusive pointer rather than copy the value to a
 * new leaf.
 *
 * If the leaf does not exist, the callback is called with a nullptr; the
 * operation must handle this (e.g., by treating it as a default_value).
 *
 * The callback may return a value, an optional value, a leaf node intrusive
 * pointer, or a nullptr. If the optional value is nullopt or if the return
 * value is nullptr, the result is nullptr. Otherwise, a leaf node pointer
 * is returned holding the value (which may be a pre-existing leaf node).
 */

// **Do not call directly.**  Use update_leaf or update_new_leaf instead.
//
// Assumes `!leaf || key == leaf->key()`.
template <typename IntegerType, typename Value, typename LeafOperation>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> update_leaf_internal(
    LeafOperation&& leaf_operation,
    IntegerType key,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> leaf) {
  auto value_or_leaf = leaf_operation(std::move(leaf));

  // Being able to return different static types from the operation
  // is significantly more convenient, and is what helps the inliner
  // eliminate all redundant paths (rather than a dynamic dispatch).
  using ValueOrLeaf = decltype(value_or_leaf);
  using ValueType = typename Value::type;
  using OptValueType = boost::optional<ValueType>;
  using LeafType = PatriciaTreeLeaf<IntegerType, Value>;
  using LeafIntrusivePtr = intrusive_ptr<LeafType>;

  constexpr bool kIsValue = std::is_same_v<ValueOrLeaf, ValueType>;
  constexpr bool kIsOptValue = std::is_same_v<ValueOrLeaf, OptValueType>;
  constexpr bool kIsLeaf = std::is_same_v<ValueOrLeaf, LeafIntrusivePtr>;
  constexpr bool kIsNullOpt = std::is_same_v<ValueOrLeaf, boost::none_t>;
  constexpr bool kIsNullptr = std::is_same_v<ValueOrLeaf, std::nullptr_t>;
  static_assert(kIsValue || kIsOptValue || kIsLeaf || kIsNullOpt || kIsNullptr,
                "ValueOrLeaf must hold a Value::type or be nullable");

  ValueType* value_ptr;
  if constexpr (kIsValue) {
    value_ptr = &value_or_leaf;
  } else if constexpr (kIsOptValue) {
    value_ptr = value_or_leaf ? &(*value_or_leaf) : nullptr;
  } else if constexpr (kIsLeaf) {
    return value_or_leaf;
  } else {
    value_ptr = nullptr;
  }

  if (!value_ptr) {
    return nullptr;
  } else if (leaf && Value::equals(*value_ptr, leaf->value())) {
    return leaf;
  } else {
    return LeafType::make(key, std::move(*value_ptr));
  }
}

// Modify a leaf by invoking the given operation on the current value,
// which will return a proposed updated value.
//
// LeafOperation may return a value, an optional value, a leaf node intrusive
// pointer, or a nullptr. If nullopt or nullptr is returned, then nullptr is
// returned. Otherwise, a leaf node pointer is returned holding the value.
//
// If a leaf node pointer was returned, it is returned directly. Otherwise if
// the returned value is equal to the existing leaf value, the existing leaf
// is preferred and returned.
template <typename IntegerType, typename Value, typename LeafOperation>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> update_leaf(
    LeafOperation&& leaf_operation,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> leaf) {
  const auto leaf_key = leaf->key();
  return update_leaf_internal<IntegerType, Value>(
      std::forward<LeafOperation>(leaf_operation), leaf_key, std::move(leaf));
}

// Update a new leaf by invoking the given operation on the default value,
// which will return a proposed updated value.
//
// LeafOperation may return a value, an optional value, a leaf node intrusive
// pointer, or a nullptr. If nullopt or nullptr is returned, then nullptr is
// returned. Otherwise, a leaf node pointer is returned holding the value.
//
// If a leaf node pointer was returned, it is returned directly. Otherwise if
// the returned value is equal to the existing leaf value, the existing leaf
// is preferred and returned.
template <typename IntegerType, typename Value, typename LeafOperation>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> update_new_leaf(
    LeafOperation&& leaf_operation, IntegerType key) {
  return update_leaf_internal<IntegerType, Value>(
      std::forward<LeafOperation>(leaf_operation), key, nullptr);
}

// Modify a key value by invoking the given operation on the current value,
// which will return a proposed updated value.
//
// LeafOperation may return a value, an optional value, a leaf node intrusive
// pointer, or a nullptr. If nullopt or nullptr is returned, then nullptr is
// returned. Otherwise, a leaf node pointer is returned holding the value.
//
// If a leaf node pointer was returned, it is returned directly. Otherwise if
// the returned value is equal to the existing leaf value, the existing leaf
// is preferred and returned.
template <typename IntegerType, typename Value, typename LeafOperation>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> update_leaf_by_key(
    LeafOperation&& leaf_operation,
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  const auto make_new_leaf = [key, &leaf_operation]() {
    return update_new_leaf<IntegerType, Value>(
        std::forward<LeafOperation>(leaf_operation), key);
  };
  if (tree == nullptr) {
    return make_new_leaf();
  }

  if (const auto* leaf = tree->as_leaf()) {
    if (key == leaf->key()) {
      return update_leaf(
          std::forward<LeafOperation>(leaf_operation),
          boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(
              tree));
    }
    auto new_leaf = make_new_leaf();
    if (new_leaf == nullptr) {
      return tree;
    } else {
      return join_trees<IntegerType, Value>(
          key, std::move(new_leaf), leaf->key(), tree);
    }
  }

  const auto* branch = tree->as_branch();
  if (match_prefix(key, branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(key, branch->branching_bit())) {
      auto new_left_tree =
          update_leaf_by_key(std::forward<LeafOperation>(leaf_operation),
                             key,
                             branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return tree;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         std::move(new_left_tree),
                         branch->right_tree());
    } else {
      auto new_right_tree =
          update_leaf_by_key(std::forward<LeafOperation>(leaf_operation),
                             key,
                             branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return tree;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         branch->left_tree(),
                         std::move(new_right_tree));
    }
  }
  auto new_leaf = make_new_leaf();
  if (new_leaf == nullptr) {
    return tree;
  } else {
    return join_trees<IntegerType, Value>(
        key, std::move(new_leaf), branch->prefix(), tree);
  }
}

// Update or insert a key value.
//
// Accepts a value, an optional value, a leaf node intrusive pointer, or a
// nullptr. If nullopt or nullptr is provided, then the leaf is removed.
// Otherwise, it is inserted or updated to hold the given value.
template <typename IntegerType, typename Value, typename ValueOrLeaf>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> upsert_leaf_by_key(
    IntegerType key,
    ValueOrLeaf value_or_leaf,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  return update_leaf_by_key(
      [&value_or_leaf](const auto&) { return std::move(value_or_leaf); },
      key,
      tree);
}

template <typename IntegerType, typename Value, typename LeafOperation>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> update_all_leafs(
    LeafOperation&& leaf_operation,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    auto leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    return update_leaf(leaf_operation, std::move(leaf));
  }
  const auto* branch = tree->as_branch();
  auto new_left_tree = update_all_leafs(leaf_operation, branch->left_tree());
  auto new_right_tree = update_all_leafs(leaf_operation, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return tree;
  } else {
    return make_branch(branch->prefix(),
                       branch->branching_bit(),
                       std::move(new_left_tree),
                       std::move(new_right_tree));
  }
}

template <typename IntegerType, typename Value, typename Visitor>
inline void visit_all_leafs(
    Visitor&& visitor,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return;
  } else if (const auto* leaf = tree->as_leaf(); leaf != nullptr) {
    visitor(leaf->data());
  } else {
    const auto* branch = tree->as_branch();
    visit_all_leafs(visitor, branch->left_tree());
    visit_all_leafs(visitor, branch->right_tree());
  }
}

template <typename IntegerType, typename Value, typename LeafCombine>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> combine_leafs(
    LeafCombine&& leaf_combine,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> other,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> leaf) {
  return update_leaf(
      [leaf_combine = fwd_capture(std::forward<LeafCombine>(leaf_combine)),
       &other](auto leaf) mutable {
        return leaf_combine.get()(std::move(leaf), std::move(other));
      },
      std::move(leaf));
}

template <typename IntegerType, typename Value, typename LeafCombine>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> combine_leafs_by_key(
    LeafCombine&& leaf_combine,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> other,
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  return update_leaf_by_key(
      [leaf_combine = fwd_capture(std::forward<LeafCombine>(leaf_combine)),
       &other](auto leaf) mutable {
        return leaf_combine.get()(std::move(leaf), std::move(other));
      },
      key,
      tree);
}

template <typename IntegerType, typename Value, typename LeafCombine>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> merge_trees(
    LeafCombine&& leaf_combine,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& s,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& t) {
  if (s == t) {
    // This conditional is what allows the union operation to complete in
    // sublinear time when the operands share some structure.
    return s;
  } else if (s == nullptr) {
    return t;
  } else if (t == nullptr) {
    return s;
  }
  // We need to check whether t is a leaf before we do the same for s.
  // Otherwise, if s and t are both leaves, we would end up inserting s into t.
  // This would violate the assumptions required by `reference_equals()`.
  if (t->is_leaf()) {
    auto t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    const auto t_key = t_leaf->key();
    return combine_leafs_by_key(leaf_combine, std::move(t_leaf), t_key, s);
  } else if (s->is_leaf()) {
    auto s_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    const auto s_key = s_leaf->key();
    return combine_leafs_by_key(leaf_combine, std::move(s_leaf), s_key, t);
  }
  const auto* s_branch = s->as_branch();
  const auto* t_branch = t->as_branch();
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
    auto new_left = merge_trees(leaf_combine, s0, t0);
    auto new_right = merge_trees(leaf_combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    } else if (new_left == t0 && new_right == t1) {
      return t;
    } else {
      return PatriciaTreeBranch<IntegerType, Value>::make(
          p, m, std::move(new_left), std::move(new_right));
    }
  } else if (m < n && match_prefix(q, p, m)) {
    // q contains p. Merge t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = merge_trees(leaf_combine, s0, t);
      if (s0 == new_left) {
        return s;
      } else {
        return PatriciaTreeBranch<IntegerType, Value>::make(
            p, m, std::move(new_left), s1);
      }
    } else {
      auto new_right = merge_trees(leaf_combine, s1, t);
      if (s1 == new_right) {
        return s;
      } else {
        return PatriciaTreeBranch<IntegerType, Value>::make(
            p, m, s0, std::move(new_right));
      }
    }
  } else if (m > n && match_prefix(p, q, n)) {
    // p contains q. Merge s with a subtree of t.
    if (is_zero_bit(p, n)) {
      auto new_left = merge_trees(leaf_combine, s, t0);
      if (t0 == new_left) {
        return t;
      } else {
        return PatriciaTreeBranch<IntegerType, Value>::make(
            q, n, std::move(new_left), t1);
      }
    } else {
      auto new_right = merge_trees(leaf_combine, s, t1);
      if (t1 == new_right) {
        return t;
      } else {
        return PatriciaTreeBranch<IntegerType, Value>::make(
            q, n, t0, std::move(new_right));
      }
    }
  }
  // The prefixes disagree.
  return join_trees(p, s, q, t);
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> use_available_leaf(
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> x,
    intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> y) {
  if (x) {
    return x;
  } else if (y) {
    return y;
  } else {
    SPARTA_THROW_EXCEPTION(internal_error()
                           << error_msg("Malformed Patricia tree"));
  }
}

template <typename IntegerType, typename Value, typename LeafCombine>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> intersect_trees(
    LeafCombine&& leaf_combine,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& s,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& t) {
  if (s == t) {
    // This conditional is what allows the intersection operation to complete in
    // sublinear time when the operands share some structure.
    return s;
  } else if (s == nullptr || t == nullptr) {
    return nullptr;
  }
  if (const auto* s_leaf = s->as_leaf()) {
    auto key_in_t = clone_leaf_pointer_by_key(s_leaf->key(), t);
    if (key_in_t == nullptr) {
      return nullptr;
    } else {
      return combine_leafs(
          leaf_combine,
          std::move(key_in_t),
          boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s));
    }
  } else if (const auto* t_leaf = t->as_leaf()) {
    auto key_in_s = clone_leaf_pointer_by_key(t_leaf->key(), s);
    if (key_in_s == nullptr) {
      return nullptr;
    } else {
      return combine_leafs(
          leaf_combine,
          std::move(key_in_s),
          boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t));
    }
  }
  const auto* s_branch = s->as_branch();
  const auto* t_branch = t->as_branch();
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
    auto new_left = intersect_trees(leaf_combine, s0, t0);
    auto new_right = intersect_trees(leaf_combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    } else {
      return make_branch(p, m, std::move(new_left), std::move(new_right));
    }
  } else if (m < n && match_prefix(q, p, m)) {
    // q contains p. Intersect t with a subtree of s.
    return intersect_trees(leaf_combine, is_zero_bit(q, m) ? s0 : s1, t);
  } else if (m > n && match_prefix(p, q, n)) {
    // p contains q. Intersect s with a subtree of t.
    return intersect_trees(leaf_combine, s, is_zero_bit(p, n) ? t0 : t1);
  }
  // The prefixes disagree.
  return nullptr;
}

template <typename IntegerType, typename Value, typename LeafCombine>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> diff_trees(
    LeafCombine&& leaf_combine,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& s,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& t) {
  if (s == t) {
    // This conditional is what allows the intersection operation to complete in
    // sublinear time when the operands share some structure.
    return nullptr;
  } else if (s == nullptr) {
    return nullptr;
  } else if (t == nullptr) {
    return s;
  }
  if (const auto* s_leaf = s->as_leaf()) {
    auto key_in_t = clone_leaf_pointer_by_key(s_leaf->key(), t);
    if (key_in_t == nullptr) {
      return s;
    } else {
      return combine_leafs(
          leaf_combine,
          std::move(key_in_t),
          boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s));
    }
  } else if (t->is_leaf()) {
    auto t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    const auto t_key = t_leaf->key();
    return combine_leafs_by_key(leaf_combine, std::move(t_leaf), t_key, s);
  }
  const auto* s_branch = s->as_branch();
  const auto* t_branch = t->as_branch();
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
    auto new_left = diff_trees(leaf_combine, s0, t0);
    auto new_right = diff_trees(leaf_combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    } else {
      return make_branch(p, m, std::move(new_left), std::move(new_right));
    }
  } else if (m < n && match_prefix(q, p, m)) {
    // q contains p. Diff t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = diff_trees(leaf_combine, s0, t);
      if (new_left == s0) {
        return s;
      } else {
        return make_branch(p, m, std::move(new_left), s1);
      }
    } else {
      auto new_right = diff_trees(leaf_combine, s1, t);
      if (new_right == s1) {
        return s;
      } else {
        return make_branch(p, m, s0, std::move(new_right));
      }
    }
  } else if (m > n && match_prefix(p, q, n)) {
    // p contains q. Diff s with a subtree of t.
    if (is_zero_bit(p, n)) {
      return diff_trees(leaf_combine, s, t0);
    } else {
      return diff_trees(leaf_combine, s, t1);
    }
  }
  // The prefixes disagree.
  return s;
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> remove_leaf_by_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  return upsert_leaf_by_key(key, nullptr, tree);
}

template <typename IntegerType, typename Value, typename Predicate>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> filter_tree(
    Predicate&& predicate,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (const auto* leaf = tree->as_leaf()) {
    return predicate(leaf->key(), leaf->value()) ? tree : nullptr;
  }
  const auto* branch = tree->as_branch();
  auto new_left_tree = filter_tree(predicate, branch->left_tree());
  auto new_right_tree = filter_tree(predicate, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return tree;
  } else {
    return make_branch(branch->prefix(),
                       branch->branching_bit(),
                       std::move(new_left_tree),
                       std::move(new_right_tree));
  }
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> erase_keys_matching(
    IntegerType key_mask,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (const auto* leaf = tree->as_leaf()) {
    if (key_mask & leaf->key()) {
      return nullptr;
    } else {
      return tree;
    }
  }
  const auto* branch = tree->as_branch();
  if (key_mask & branch->prefix()) {
    return nullptr;
  } else if (key_mask < branch->branching_bit()) {
    return tree;
  }
  auto new_left_tree = erase_keys_matching(key_mask, branch->left_tree());
  auto new_right_tree = erase_keys_matching(key_mask, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return tree;
  } else {
    return make_branch(branch->prefix(),
                       branch->branching_bit(),
                       std::move(new_left_tree),
                       std::move(new_right_tree));
  }
}

template <typename Key, typename Value>
class PatriciaTreeCore {
 public:
  using Codec = pt_util::Codec<Key>;
  using IntegerType = typename Codec::IntegerType;
  using ValueType = typename Value::type;
  using IteratorType = PatriciaTreeIterator<Key, Value>;

  static_assert(std::is_same_v<decltype(Value::default_value()), ValueType>,
                "Value::default_value() does not exist");
  static_assert(std::is_same_v<decltype(Value::is_default_value(
                                   std::declval<ValueType>())),
                               bool>,
                "Value::is_default_value() does not exist");
  static_assert(
      std::is_same_v<decltype(Value::equals(std::declval<ValueType>(),
                                            std::declval<ValueType>())),
                     bool>,
      "Value::equals() does not exist");
  static_assert(std::is_same_v<decltype(Value::default_value_kind),
                               const AbstractValueKind>,
                "Value::default_value_kind does not exist");

  inline bool empty() const { return m_tree == nullptr; }

  inline size_t size() const {
    size_t s = 0;
    std::for_each(begin(), end(), [&s](const auto&) { ++s; });
    return s;
  }

  inline size_t max_size() const {
    return std::numeric_limits<IntegerType>::max();
  }

  inline IteratorType begin() const { return IteratorType(m_tree); }

  inline IteratorType end() const { return IteratorType(); }

  inline bool contains(Key key) const {
    return contains_leaf_with_key(Codec::encode(key), m_tree);
  }

  // Returns nullptr if the key is mapped to the default value.
  inline const ValueType* find(Key key) const {
    return find_value_by_key(Codec::encode(key), m_tree);
  }

  inline const ValueType& at(Key key) const {
    if (const auto* value = find_value_by_key(Codec::encode(key), m_tree)) {
      return *value;
    }

    static const ValueType default_value = Value::default_value();
    return default_value;
  }

  /*
   * If the tree is a leaf, returns a pointer on the key.
   * Otherwise, returns nullptr.
   */
  inline const Key* as_leaf_key() const {
    if (m_tree == nullptr) {
      return nullptr;
    }
    const auto* leaf = m_tree->as_leaf();
    if (leaf == nullptr) {
      return nullptr;
    }
    // Taking the address of the return value of `Codec::decode` is safe here
    // since it has type `const IntegerType& -> const Key&`.
    return &Codec::decode(leaf->key());
  }

  inline bool is_subset_of(const PatriciaTreeCore& other) const {
    return pt_core::is_tree_subset_of(m_tree, other.m_tree);
  }

  template <typename Compare>
  inline bool leq(const PatriciaTreeCore& other, Compare compare) const {
    return pt_core::is_tree_leq(
        m_tree, other.m_tree, std::forward<Compare>(compare));
  }

  inline bool equals(const PatriciaTreeCore& other) const {
    return pt_core::is_tree_equal(m_tree, other.m_tree);
  }

  inline bool reference_equals(const PatriciaTreeCore& other) const {
    return m_tree == other.m_tree;
  }

  template <typename ValueOrLeaf>
  inline void upsert(Key key, ValueOrLeaf value_or_leaf) {
    m_tree = pt_core::upsert_leaf_by_key(
        Codec::encode(key), std::move(value_or_leaf), m_tree);
  }

  template <typename LeafOperation>
  inline void update(LeafOperation&& leaf_operation, Key key) {
    m_tree =
        pt_core::update_leaf_by_key(std::forward<LeafOperation>(leaf_operation),
                                    Codec::encode(key),
                                    m_tree);
  }

  template <typename LeafOperation>
  inline bool update_all_leafs(LeafOperation&& leaf_operation) {
    auto new_tree = pt_core::update_all_leafs(
        std::forward<LeafOperation>(leaf_operation), m_tree);
    auto old_tree = std::exchange(m_tree, std::move(new_tree));

    return m_tree != old_tree;
  }

  template <typename Visitor>
  inline void visit_all_leafs(Visitor&& visitor) const {
    pt_core::visit_all_leafs(
        [visitor = fwd_capture(std::forward<Visitor>(visitor))](
            const auto& data) mutable { visitor.get()(Codec::decode(data)); },
        m_tree);
  }

  template <typename LeafCombine>
  inline void merge(LeafCombine&& leaf_combine, const PatriciaTreeCore& other) {
    m_tree = pt_core::merge_trees(
        std::forward<LeafCombine>(leaf_combine), m_tree, other.m_tree);
  }

  template <typename LeafCombine>
  inline void intersect(LeafCombine&& leaf_combine,
                        const PatriciaTreeCore& other) {
    m_tree = pt_core::intersect_trees(
        std::forward<LeafCombine>(leaf_combine), m_tree, other.m_tree);
  }

  template <typename LeafCombine>
  inline void diff(LeafCombine&& leaf_combine, const PatriciaTreeCore& other) {
    m_tree = pt_core::diff_trees(
        std::forward<LeafCombine>(leaf_combine), m_tree, other.m_tree);
  }

  inline void remove(Key key) {
    m_tree = pt_core::remove_leaf_by_key(Codec::encode(key), m_tree);
  }

  template <typename Predicate>
  inline void filter(Predicate&& predicate) {
    m_tree = pt_core::filter_tree(
        [predicate = fwd_capture(std::forward<Predicate>(predicate))](
            IntegerType key, const ValueType& value) mutable {
          return predicate.get()(Codec::decode(key), value);
        },
        m_tree);
  }

  // Erases all entries where keys and :key_mask share common bits.
  inline bool erase_all_matching(Key key_mask) {
    auto new_tree =
        pt_core::erase_keys_matching(Codec::encode(key_mask), m_tree);
    auto old_tree = std::exchange(m_tree, std::move(new_tree));

    return m_tree != old_tree;
  }

  inline size_t hash() const { return m_tree == nullptr ? 0 : m_tree->hash(); }

  inline void clear() { m_tree.reset(); }

 private:
  boost::intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> m_tree;
};

} // namespace pt_core

} // namespace sparta
