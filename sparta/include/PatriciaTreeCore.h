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
#include <limits>
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

template <typename Key, typename Value>
class PatriciaTreeCore {
 public:
  using Codec = pt_util::Codec<Key>;
  using IntegerType = typename Codec::IntegerType;
  using ValueType = typename Value::type;

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

  inline size_t max_size() const {
    return std::numeric_limits<IntegerType>::max();
  }

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
