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

#include "AbstractDomain.h"
#include "Exceptions.h"
#include "PatriciaTreeUtil.h"

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
struct SimpleValue {
  using type = T;

  static T default_value() { return T(); }

  static bool is_default_value(const T& t) { return t == T(); }

  static bool equals(const T& a, const T& b) { return a == b; }
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

  size_t hash() const {
    if (is_leaf()) {
      return static_cast<const LeafType*>(this)->hash();
    } else {
      return static_cast<const BranchType*>(this)->hash();
    }
  }

 protected:
  PatriciaTreeNode(bool is_leaf) : m_reference_count(is_leaf ? LEAF_MASK : 0) {}
  ~PatriciaTreeNode() = default;

 private:
  friend void intrusive_ptr_add_ref(const PatriciaTreeNode* p) {
    p->m_reference_count.fetch_add(1, std::memory_order_relaxed);
  }

  friend void intrusive_ptr_release(const PatriciaTreeNode* p) {
    size_t prev_reference_count =
        p->m_reference_count.fetch_sub(1, std::memory_order_release);
    if ((prev_reference_count & ~LEAF_MASK) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      if (prev_reference_count & LEAF_MASK) {
        delete static_cast<const LeafType*>(p);
      } else {
        delete static_cast<const BranchType*>(p);
      }
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
    return new PatriciaTreeLeaf(key, std::move(value));
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
    return new PatriciaTreeBranch(prefix, branching_bit, std::move(left_tree),
                                  std::move(right_tree));
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
    RUNTIME_CHECK(m_leaf != nullptr, undefined_operation());

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
    while (t->is_branch()) {
      const auto* branch = static_cast<const BranchType*>(t);
      m_stack.push(branch);

      t = branch->left_tree().get();
      // A branch node always has two children.
      RUNTIME_CHECK(t != nullptr, internal_error());
    }

    m_leaf = static_cast<const LeafType*>(t);
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
inline intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> find_leaf(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    auto leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key == leaf->key()) {
      return leaf;
    } else {
      return nullptr;
    }
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (is_zero_bit(key, branch->branching_bit())) {
    return find_leaf(key, branch->left_tree());
  } else {
    return find_leaf(key, branch->right_tree());
  }
}

// Returns a pointer to the leaf's value if present, else nullptr.
template <typename IntegerType, typename Value>
inline const typename Value::type* find_key_value(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  auto leaf = find_leaf(key, tree);
  return leaf ? &leaf->value() : nullptr;
}

template <typename IntegerType, typename Value>
inline bool contains_key(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  return find_leaf(key, tree) != nullptr;
}

template <typename IntegerType, typename Value>
inline bool is_subset_of(
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

  if (tree1->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree1);
    return contains_key(leaf->key(), tree2);
  } else if (tree2->is_leaf()) {
    return false;
  }

  const auto& branch1 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree1);
  const auto& branch2 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree2);
  if (branch1->prefix() == branch2->prefix() &&
      branch1->branching_bit() == branch2->branching_bit()) {
    return is_subset_of(branch1->left_tree(), branch2->left_tree()) &&
           is_subset_of(branch1->right_tree(), branch2->right_tree());
  } else if (branch1->branching_bit() > branch2->branching_bit() &&
             match_prefix(branch1->prefix(), branch2->prefix(),
                          branch2->branching_bit())) {
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

/* Assumes Value::default_value() is either Top or Bottom */
template <typename IntegerType, typename Value>
inline bool leq(const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& s,
                const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& t) {
  using ValueType = typename Value::type;
  constexpr bool kHasLeq =
      std::is_same_v<decltype(Value::leq(std::declval<ValueType>(),
                                         std::declval<ValueType>())),
                     bool>;
  constexpr bool kIsAbstractDomain =
      std::is_base_of_v<AbstractDomain<ValueType>, ValueType>;
  static_assert(!kHasLeq || kIsAbstractDomain,
                "Value::leq() is defined, but Value::type is not an "
                "implementation of AbstractDomain");

  RUNTIME_CHECK(Value::default_value().is_top() ||
                    Value::default_value().is_bottom(),
                undefined_operation());

  if (s == t) {
    // This condition allows the leq operation to run in sublinear time when
    // comparing Patricia trees that share some structure.
    return true;
  } else if (s == nullptr) {
    return Value::default_value().is_bottom();
  } else if (t == nullptr) {
    return Value::default_value().is_top();
  }

  if (s->is_leaf()) {
    const auto& s_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);

    if (t->is_branch()) {
      // t has at least one non-default binding that s doesn't have.
      if (Value::default_value().is_top()) {
        // The non-default binding in t can never be <= Top.
        return false;
      }

      // Otherwise, find if t contains s.
      // The missing bindings in s are bound to Bottom in this case. Even if we
      // know t contains strictly more bindings than s, they all satisfy the leq
      // condition. For each key k in t but not in s, s[k] == Bottom <= t[k]
      // always hold.
      auto* t_value = find_key_value(s_leaf->key(), t);
      if (t_value == nullptr) {
        // Always false if default_value is Bottom, which we already assume.
        return false;
      } else {
        return Value::leq(s_leaf->value(), *t_value);
      }
    }

    // Both nodes are leaves. s leq to t iff
    // key(s) == key(t) && value(s) <= value(t).
    const auto& t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    return s_leaf->key() == t_leaf->key() &&
           Value::leq(s_leaf->value(), t_leaf->value());
  } else if (t->is_leaf()) {
    // s has at least one non-default binding that t doesn't have.
    if (Value::default_value().is_bottom()) {
      // There exists a key such that s[key] != Bottom and t[key] == Bottom.
      return false;
    }

    const auto& t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    auto* s_value = find_key_value(t_leaf->key(), s);
    if (s_value == nullptr) {
      // Always false if default_value is Top, which we already assume.
      return false;
    } else {
      return Value::leq(*s_value, t_leaf->value());
    }
  }

  // Neither s nor t is a leaf.
  const auto& s_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(s);
  const auto& t_branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(t);
  IntegerType m = s_branch->branching_bit();
  IntegerType n = t_branch->branching_bit();
  IntegerType p = s_branch->prefix();
  IntegerType q = t_branch->prefix();
  const auto& s0 = s_branch->left_tree();
  const auto& s1 = s_branch->right_tree();
  const auto& t0 = t_branch->left_tree();
  const auto& t1 = t_branch->right_tree();
  if (m == n && p == q) {
    // The two trees have the same prefix, compare each subtrees.
    return leq(s0, t0) && leq(s1, t1);
  } else if (m < n && match_prefix(q, p, m)) {
    // The tree t only contains bindings present in a subtree of s, and s has
    // bindings not present in t.
    return Value::default_value().is_top() &&
           leq(is_zero_bit(q, m) ? s0 : s1, t);
  } else if (m > n && match_prefix(p, q, n)) {
    // The tree s only contains bindings present in a subtree of t, and t has
    // bindings not present in s.
    return Value::default_value().is_bottom() &&
           leq(s, is_zero_bit(p, n) ? t0 : t1);
  } else {
    // s and t both have bindings that are not present in the other tree.
    return false;
  }
}

template <typename IntegerType, typename Value>
inline bool equals(
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree1,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the equality test to run in sublinear time
    // when comparing Patricia trees that share some structure.
    return true;
  } else if (tree1 == nullptr || tree2 == nullptr) {
    return false;
  }

  if (tree1->is_leaf()) {
    if (tree2->is_branch()) {
      return false;
    }
    const auto& leaf1 =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree1);
    const auto& leaf2 =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree2);
    return leaf1->key() == leaf2->key() &&
           Value::equals(leaf1->value(), leaf2->value());
  } else if (tree2->is_leaf()) {
    return false;
  }

  const auto& branch1 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree1);
  const auto& branch2 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree2);
  return branch1->hash() == branch2->hash() &&
         branch1->prefix() == branch2->prefix() &&
         branch1->branching_bit() == branch2->branching_bit() &&
         equals(branch1->left_tree(), branch2->left_tree()) &&
         equals(branch1->right_tree(), branch2->right_tree());
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeBranch<IntegerType, Value>> join(
    IntegerType prefix0,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree0,
    IntegerType prefix1,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree1) {
  IntegerType m = get_branching_bit(prefix0, prefix1);
  if (is_zero_bit(prefix0, m)) {
    return PatriciaTreeBranch<IntegerType, Value>::make(mask(prefix0, m), m,
                                                        tree0, tree1);
  } else {
    return PatriciaTreeBranch<IntegerType, Value>::make(mask(prefix0, m), m,
                                                        tree1, tree0);
  }
}

// This function is used to prevent the creation of branch nodes with only one
// child. Returns a subtree if one of left or right is null, else a new branch.
template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> make_branch(
    IntegerType prefix,
    IntegerType branching_bit,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& left_tree,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& right_tree) {
  if (left_tree == nullptr) {
    return right_tree;
  } else if (right_tree == nullptr) {
    return left_tree;
  } else {
    return PatriciaTreeBranch<IntegerType, Value>::make(prefix, branching_bit,
                                                        left_tree, right_tree);
  }
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> remove(
    IntegerType key,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }

  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key == leaf->key()) {
      return nullptr;
    } else {
      return leaf;
    }
  }

  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (match_prefix(key, branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(key, branch->branching_bit())) {
      auto new_left_tree = remove(key, branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return branch;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         new_left_tree,
                         branch->right_tree());
    } else {
      auto new_right_tree = remove(key, branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return branch;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         branch->left_tree(),
                         new_right_tree);
    }
  }
  return branch;
}

template <typename IntegerType, typename Value>
inline intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> erase_all_matching(
    IntegerType key_mask,
    const intrusive_ptr<PatriciaTreeNode<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key_mask & leaf->key()) {
      return nullptr;
    } else {
      return tree;
    }
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (key_mask & branch->prefix()) {
    return nullptr;
  } else if (key_mask < branch->branching_bit()) {
    return branch;
  }
  auto new_left_tree = erase_all_matching(key_mask, branch->left_tree());
  auto new_right_tree = erase_all_matching(key_mask, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return branch;
  } else {
    return make_branch(branch->prefix(), branch->branching_bit(), new_left_tree,
                       new_right_tree);
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
    return contains_key(Codec::encode(key), m_tree);
  }

  inline const ValueType& at(Key key) const {
    if (const auto* value = find_key_value(Codec::encode(key), m_tree)) {
      return *value;
    }

    static const ValueType default_value = Value::default_value();
    return default_value;
  }

  inline bool is_subset_of(const PatriciaTreeCore& other) const {
    return pt_core::is_subset_of(m_tree, other.m_tree);
  }

  inline bool leq(const PatriciaTreeCore& other) const {
    return pt_core::leq(m_tree, other.m_tree);
  }

  inline bool equals(const PatriciaTreeCore& other) const {
    return pt_core::equals(m_tree, other.m_tree);
  }

  inline bool reference_equals(const PatriciaTreeCore& other) const {
    return m_tree == other.m_tree;
  }

  inline void remove(Key key) {
    m_tree = pt_core::remove(Codec::encode(key), m_tree);
  }

  // Erases all entries where keys and :key_mask share common bits.
  inline bool erase_all_matching(Key key_mask) {
    auto new_tree =
        pt_core::erase_all_matching(Codec::encode(key_mask), m_tree);
    auto old_tree = std::exchange(m_tree, std::move(new_tree));

    return m_tree != old_tree;
  }

  inline size_t hash() const { return m_tree == nullptr ? 0 : m_tree->hash(); }

  inline void clear() { m_tree.reset(); }

  // Public for now, until more of the implementation is merged into one.
  boost::intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> m_tree;
};

} // namespace pt_core

} // namespace sparta
