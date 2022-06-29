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

  inline bool reference_equals(const PatriciaTreeCore& other) const {
    return m_tree == other.m_tree;
  }

  inline size_t hash() const { return m_tree == nullptr ? 0 : m_tree->hash(); }

  inline void clear() { m_tree.reset(); }

  // Public for now, until more of the implementation is merged into one.
  boost::intrusive_ptr<PatriciaTreeNode<IntegerType, Value>> m_tree;
};

} // namespace pt_core

} // namespace sparta
