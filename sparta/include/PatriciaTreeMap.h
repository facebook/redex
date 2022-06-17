/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <ostream>
#include <stack>
#include <type_traits>
#include <utility>

#include <boost/intrusive_ptr.hpp>

#include "AbstractDomain.h"
#include "PatriciaTreeUtil.h"

// Forward declarations
namespace sparta {

template <typename Key, typename ValueType, typename Value>
class PatriciaTreeMap;

} // namespace sparta

template <typename Key, typename ValueType, typename Value>
std::ostream& operator<<(
    std::ostream&,
    const typename sparta::PatriciaTreeMap<Key, ValueType, Value>&);

namespace sparta {

// Forward declarations.
namespace ptmap_impl {

template <typename IntegerType, typename Value>
class PatriciaTree;

template <typename IntegerType, typename Value>
class PatriciaTreeBranch;

template <typename IntegerType, typename Value>
class PatriciaTreeLeaf;

template <typename IntegerType, typename Value>
class PatriciaTreeIterator;

template <typename T>
using CombiningFunction = std::function<T(const T&, const T&)>;

template <typename Value>
using MappingFunction = std::function<Value(const Value&)>;

template <typename IntegerType, typename Value>
inline const typename Value::type* find_value(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree);

template <typename IntegerType, typename Value>
inline bool leq(
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree2);

template <typename IntegerType, typename Value>
inline bool equals(
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree2);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> combine_new_leaf(
    const CombiningFunction<typename Value::type>& combine,
    IntegerType key,
    const typename Value::type& value);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> update(
    const CombiningFunction<typename Value::type>& combine,
    IntegerType key,
    const typename Value::type& value,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> map(
    const MappingFunction<typename Value::type>& f,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>
erase_all_matching(
    IntegerType key_mask,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> merge(
    const CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> intersect(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> diff(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

template <typename T>
T snd(const T&, const T& second) {
  return second;
}

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

} // namespace ptmap_impl

/*
 * This structure implements a map of integer/pointer keys and AbstractDomain
 * values. It's based on the following paper:
 *
 *   C. Okasaki, A. Gill. Fast Mergeable Integer Maps. In Workshop on ML (1998).
 *
 * See PatriciaTreeSet.h for more details about Patricia trees.
 *
 * This implementation differs from the paper in that we allow for a special
 * default value, which is never explicitly represented in the map. When using
 * Patricia tree maps with AbstractDomain values, this allows us to better
 * optimize operations like meet, join, and leq. It also makes it easy for us to
 * save space by implicitly mapping all unbound keys to the default value. As a
 * consequence, the default value must be either Top or Bottom.
 *
 * Value is a structure that should contain the following components:
 *
 *   struct Value {
 *     // The type of elements used as values in the map.
 *     using type = ...;
 *
 *     // Returns the default value.
 *     static type default_value();
 *
 *     // Tests whether a value is the default value.
 *     static bool is_default_value(const type& x);
 *
 *     // The equality predicate for values.
 *     static bool equals(const type& x, const type& y);
 *
 *     // A partial order relation over values. In order to use the lifted
 *     // partial order relation over maps PatriciaTreeMap::leq(), this method
 *     // must be implemented. Additionally, value::type must be an
 *     // implementation of an AbstractDomain.
 *     static bool leq(const type& x, const type& y);
 *   }
 *
 * Patricia trees can only handle unsigned integers. Arbitrary objects can be
 * accommodated as long as they are represented as pointers. Our implementation
 * of Patricia-tree maps can transparently operate on keys that are either
 * unsigned integers or pointers to objects.
 */
template <typename Key,
          typename ValueType,
          typename Value = ptmap_impl::SimpleValue<ValueType>>
class PatriciaTreeMap final {
 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename Value::type;
  using value_type = std::pair<const Key, mapped_type>;
  using iterator = ptmap_impl::PatriciaTreeIterator<Key, Value>;
  using const_iterator = iterator;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const mapped_type&;
  using const_pointer = const mapped_type*;

  using IntegerType =
      typename std::conditional_t<std::is_pointer<Key>::value, uintptr_t, Key>;
  using combining_function = ptmap_impl::CombiningFunction<mapped_type>;
  using mapping_function = ptmap_impl::MappingFunction<mapped_type>;

  ~PatriciaTreeMap() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_same<ValueType, typename Value::type>::value,
                  "ValueType must be equal to Value::type");
    static_assert(
        std::is_same<decltype(Value::default_value()), mapped_type>::value,
        "Value::default_value() does not exist");
    static_assert(std::is_same<decltype(Value::is_default_value(
                                   std::declval<mapped_type>())),
                               bool>::value,
                  "Value::is_default_value() does not exist");
    static_assert(
        std::is_same<decltype(Value::equals(std::declval<mapped_type>(),
                                            std::declval<mapped_type>())),
                     bool>::value,
        "Value::equals() does not exist");
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

  const mapped_type& at(Key key) const {
    const mapped_type* value = ptmap_impl::find_value(encode(key), m_tree);
    if (value == nullptr) {
      static const mapped_type default_value = Value::default_value();
      return default_value;
    }
    return *value;
  }

  bool leq(const PatriciaTreeMap& other) const {
    static_assert(!std::is_same<decltype(Value::leq(
                                    std::declval<typename Value::type>(),
                                    std::declval<typename Value::type>())),
                                bool>::value ||
                      std::is_base_of<AbstractDomain<typename Value::type>,
                                      typename Value::type>::value,
                  "Value::leq() is defined, but Value::type is not an "
                  "implementation of AbstractDomain");
    return ptmap_impl::leq<IntegerType>(m_tree, other.m_tree);
  }

  bool equals(const PatriciaTreeMap& other) const {
    return ptmap_impl::equals<IntegerType>(m_tree, other.m_tree);
  }

  friend bool operator==(const PatriciaTreeMap& m1, const PatriciaTreeMap& m2) {
    return m1.equals(m2);
  }

  friend bool operator!=(const PatriciaTreeMap& m1, const PatriciaTreeMap& m2) {
    return !m1.equals(m2);
  }

  /*
   * This faster equality predicate can be used to check whether a sequence of
   * in-place modifications leaves a Patricia-tree map unchanged. For comparing
   * two arbitrary Patricia-tree maps, one needs to use the `equals()`
   * predicate.
   *
   * Example:
   *
   *   PatriciaTreeMap<...> m1, m2;
   *   ...
   *   m2 = m1;
   *   m2.union_with(...);
   *   m2.update(...);
   *   m2.intersection_with(...);
   *   if (m2.reference_equals(m1)) { // This is equivalent to m2.equals(m1)
   *     ...
   *   }
   */
  bool reference_equals(const PatriciaTreeMap& other) const {
    return m_tree == other.m_tree;
  }

  PatriciaTreeMap& update(
      const std::function<mapped_type(const mapped_type&)>& operation,
      Key key) {
    m_tree = ptmap_impl::update<IntegerType, Value>(
        [&operation](const mapped_type& x, const mapped_type&) {
          return operation(x);
        },
        encode(key),
        Value::default_value(),
        m_tree);
    return *this;
  }

  bool map(const mapping_function& f) {
    auto new_tree = ptmap_impl::map<IntegerType, Value>(f, m_tree);
    bool res = new_tree != m_tree;
    m_tree = new_tree;
    return res;
  }

  bool erase_all_matching(Key key_mask) {
    auto new_tree = ptmap_impl::erase_all_matching<IntegerType, Value>(
        encode(key_mask), m_tree);
    bool res = new_tree != m_tree;
    m_tree = new_tree;
    return res;
  }

  PatriciaTreeMap& insert_or_assign(Key key, const mapped_type& value) {
    m_tree = ptmap_impl::update<IntegerType, Value>(
        ptmap_impl::snd<mapped_type>, encode(key), value, m_tree);
    return *this;
  }

  PatriciaTreeMap& union_with(const combining_function& combine,
                              const PatriciaTreeMap& other) {
    m_tree =
        ptmap_impl::merge<IntegerType, Value>(combine, m_tree, other.m_tree);
    return *this;
  }

  PatriciaTreeMap& intersection_with(const combining_function& combine,
                                     const PatriciaTreeMap& other) {
    m_tree = ptmap_impl::intersect<IntegerType, Value>(
        combine, m_tree, other.m_tree);
    return *this;
  }

  // Requires that `combine(bottom, ...) = bottom`.
  PatriciaTreeMap& difference_with(const combining_function& combine,
                                   const PatriciaTreeMap& other) {
    m_tree =
        ptmap_impl::diff<IntegerType, Value>(combine, m_tree, other.m_tree);
    return *this;
  }

  PatriciaTreeMap get_union_with(const combining_function& combine,
                                 const PatriciaTreeMap& other) const {
    auto result = *this;
    result.union_with(combine, other);
    return result;
  }

  PatriciaTreeMap get_intersection_with(const combining_function& combine,
                                        const PatriciaTreeMap& other) const {
    auto result = *this;
    result.intersection_with(combine, other);
    return result;
  }

  PatriciaTreeMap get_difference_with(const combining_function& combine,
                                      const PatriciaTreeMap& other) const {
    auto result = *this;
    result.difference_with(combine, other);
    return result;
  }

  void clear() { m_tree.reset(); }

 private:
  // These functions are used to handle the type conversions required when
  // manipulating maps with pointer keys. The first parameter is necessary to
  // make template deduction work.
  template <typename T = Key,
            typename std::enable_if_t<std::is_pointer<T>::value, int> = 0>
  static uintptr_t encode(Key x) {
    return reinterpret_cast<uintptr_t>(x);
  }

  template <typename T = Key,
            typename std::enable_if_t<!std::is_pointer<T>::value, int> = 0>
  static Key encode(Key x) {
    return x;
  }

  template <typename T = Key,
            typename std::enable_if_t<std::is_pointer<T>::value, int> = 0>
  static Key decode(uintptr_t x) {
    return reinterpret_cast<Key>(x);
  }

  template <typename T = Key,
            typename std::enable_if_t<!std::is_pointer<T>::value, int> = 0>
  static Key decode(Key x) {
    return x;
  }

  template <typename T = Key,
            typename std::enable_if_t<std::is_pointer<T>::value, int> = 0>
  static const typename std::remove_pointer<T>::type& deref(Key x) {
    return *x;
  }

  template <typename T = Key,
            typename std::enable_if_t<!std::is_pointer<T>::value, int> = 0>
  static Key deref(Key x) {
    return x;
  }

  boost::intrusive_ptr<ptmap_impl::PatriciaTree<IntegerType, Value>> m_tree;

  template <typename T, typename VT, typename V>
  friend std::ostream& ::operator<<(std::ostream&,
                                    const PatriciaTreeMap<T, VT, V>&);

  template <typename T, typename V>
  friend class ptmap_impl::PatriciaTreeIterator;
};

} // namespace sparta

template <typename Key, typename ValueType, typename Value>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::PatriciaTreeMap<Key, ValueType, Value>& s) {
  using namespace sparta;
  o << "{";
  for (auto it = s.begin(); it != s.end(); ++it) {
    o << PatriciaTreeMap<Key, ValueType, Value>::deref(it->first) << " -> "
      << it->second;
    if (std::next(it) != s.end()) {
      o << ", ";
    }
  }
  o << "}";
  return o;
}

namespace sparta {

namespace ptmap_impl {

using namespace pt_util;

template <typename IntegerType, typename Value>
class PatriciaTree {
 public:
  // A Patricia tree is an immutable structure.
  PatriciaTree& operator=(const PatriciaTree& other) = delete;

  ~PatriciaTree() {
    // The destructor is the only method that is guaranteed to be created when
    // a class template is instantiated. This is a good place to perform all
    // the sanity checks on the template parameters.
    static_assert(std::is_unsigned<IntegerType>::value,
                  "IntegerType is not an unsigned arihmetic type");
  }

  bool is_leaf() const {
    return m_reference_count.load(std::memory_order_relaxed) & LEAF_MASK;
  }

  bool is_branch() const { return !is_leaf(); }

  friend void intrusive_ptr_add_ref(const PatriciaTree<IntegerType, Value>* p) {
    p->m_reference_count.fetch_add(1, std::memory_order_relaxed);
  }

  friend void intrusive_ptr_release(const PatriciaTree<IntegerType, Value>* p) {
    size_t prev_reference_count =
        p->m_reference_count.fetch_sub(1, std::memory_order_release);
    if ((prev_reference_count & ~LEAF_MASK) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      if (prev_reference_count & LEAF_MASK) {
        delete static_cast<const PatriciaTreeLeaf<IntegerType, Value>*>(p);
      } else {
        delete static_cast<const PatriciaTreeBranch<IntegerType, Value>*>(p);
      }
    }
  }

 protected:
  PatriciaTree(bool is_leaf) : m_reference_count(is_leaf ? LEAF_MASK : 0) {}

 private:
  // We are stealing the highest bit of our reference counter to indicate
  // whether this tree is a leaf (or, otherwise, branch).
  static constexpr size_t LEAF_MASK = (static_cast<size_t>(1))
                                      << (sizeof(size_t) * 8 - 1);
  mutable std::atomic<size_t> m_reference_count;
};

template <typename IntegerType, typename Value>
class PatriciaTreeBranch final : public PatriciaTree<IntegerType, Value> {
 public:
  PatriciaTreeBranch(
      IntegerType prefix,
      IntegerType branching_bit,
      boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> left_tree,
      boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> right_tree)
      : PatriciaTree<IntegerType, Value>(/* is_leaf */ false),
        m_prefix(prefix),
        m_stacking_bit(branching_bit),
        m_left_tree(std::move(left_tree)),
        m_right_tree(std::move(right_tree)) {}

  IntegerType prefix() const { return m_prefix; }

  IntegerType branching_bit() const { return m_stacking_bit; }

  const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& left_tree()
      const {
    return m_left_tree;
  }

  const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& right_tree()
      const {
    return m_right_tree;
  }

  static boost::intrusive_ptr<PatriciaTreeBranch<IntegerType, Value>> make(
      IntegerType prefix,
      IntegerType branching_bit,
      boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> left_tree,
      boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> right_tree) {
    return new PatriciaTreeBranch<IntegerType, Value>(
        prefix, branching_bit, std::move(left_tree), std::move(right_tree));
  }

 private:
  IntegerType m_prefix;
  IntegerType m_stacking_bit;
  boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> m_left_tree;
  boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> m_right_tree;
};

template <typename IntegerType, typename Value>
class PatriciaTreeLeaf final : public PatriciaTree<IntegerType, Value> {
 public:
  using mapped_type = typename Value::type;

  explicit PatriciaTreeLeaf(IntegerType key, const mapped_type& value)
      : PatriciaTree<IntegerType, Value>(/* is_leaf */ true),
        m_pair(key, value) {}

  const IntegerType& key() const { return m_pair.first; }

  const mapped_type& value() const { return m_pair.second; }

  static boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> make(
      IntegerType key, const mapped_type& value) {
    return new PatriciaTreeLeaf<IntegerType, Value>(key, value);
  }

 private:
  std::pair<IntegerType, mapped_type> m_pair;

  template <typename T, typename V>
  friend class ptmap_impl::PatriciaTreeIterator;
};

template <typename IntegerType, typename Value>
boost::intrusive_ptr<PatriciaTreeBranch<IntegerType, Value>> join(
    IntegerType prefix0,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree0,
    IntegerType prefix1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree1) {
  IntegerType m = get_branching_bit(prefix0, prefix1);
  if (is_zero_bit(prefix0, m)) {
    return PatriciaTreeBranch<IntegerType, Value>::make(
        mask(prefix0, m), m, tree0, tree1);
  } else {
    return PatriciaTreeBranch<IntegerType, Value>::make(
        mask(prefix0, m), m, tree1, tree0);
  }
}

// This function is used to prevent the creation of branch nodes with only one
// child.
template <typename IntegerType, typename Value>
boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> make_branch(
    IntegerType prefix,
    IntegerType branching_bit,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& left_tree,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& right_tree) {
  if (left_tree == nullptr) {
    return right_tree;
  }
  if (right_tree == nullptr) {
    return left_tree;
  }
  return PatriciaTreeBranch<IntegerType, Value>::make(
      prefix, branching_bit, left_tree, right_tree);
}

// Tries to find the value corresponding to :key. Returns null if the key is
// not present in :tree.
template <typename IntegerType, typename Value>
inline const typename Value::type* find_value(
    IntegerType key,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key == leaf->key()) {
      return &leaf->value();
    }
    return nullptr;
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (is_zero_bit(key, branch->branching_bit())) {
    return find_value(key, branch->left_tree());
  } else {
    return find_value(key, branch->right_tree());
  }
}

/* Assumes Value::default_value() is either Top or Bottom */
template <typename IntegerType, typename Value>
inline bool leq(
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t) {

  RUNTIME_CHECK(Value::default_value().is_top() ||
                    Value::default_value().is_bottom(),
                undefined_operation());

  if (s == t) {
    // This condition allows the leq operation to run in sublinear time when
    // comparing Patricia trees that share some structure.
    return true;
  }
  if (s == nullptr) {
    return Value::default_value().is_bottom();
  }
  if (t == nullptr) {
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
      auto* t_value = find_value(s_leaf->key(), t);
      if (t_value == nullptr) {
        // Always false if default_value is Bottom, which we already assume.
        return false;
      }
      return Value::leq(s_leaf->value(), *t_value);
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
    auto* s_value = find_value(t_leaf->key(), s);
    if (s_value == nullptr) {
      // Always false if default_value is Top, which we already assume.
      return false;
    }
    return Value::leq(*s_value, t_leaf->value());
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
  }
  if (m < n && match_prefix(q, p, m)) {
    // The tree t only contains bindings present in a subtree of s, and s has
    // bindings not present in t.
    return Value::default_value().is_top() &&
           leq(is_zero_bit(q, m) ? s0 : s1, t);
  }
  if (m > n && match_prefix(p, q, n)) {
    // The tree s only contains bindings present in a subtree of t, and t has
    // bindings not present in s.
    return Value::default_value().is_bottom() &&
           leq(s, is_zero_bit(p, n) ? t0 : t1);
  }
  // s and t both have bindings that are not present in the other tree.
  return false;
}

// A Patricia tree is a canonical representation of the set of keys it contains.
// Hence, set equality is equivalent to structural equality of Patricia trees.
template <typename IntegerType, typename Value>
inline bool equals(
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree1,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree2) {
  if (tree1 == tree2) {
    // This conditions allows the equality test to run in sublinear time when
    // comparing Patricia trees that share some structure.
    return true;
  }
  if (tree1 == nullptr) {
    return tree2 == nullptr;
  }
  if (tree2 == nullptr) {
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
  }
  if (tree2->is_leaf()) {
    return false;
  }
  const auto& branch1 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree1);
  const auto& branch2 =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree2);
  return branch1->prefix() == branch2->prefix() &&
         branch1->branching_bit() == branch2->branching_bit() &&
         equals(branch1->left_tree(), branch2->left_tree()) &&
         equals(branch1->right_tree(), branch2->right_tree());
}

// Finds the value corresponding to :key in the tree and replaces its bound
// value with combine(bound_value, :value). Note that the existing value is
// always the first parameter to :combine and the new value is the second.
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> update(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    IntegerType key,
    const typename Value::type& value,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return combine_new_leaf<IntegerType, Value>(combine, key, value);
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key == leaf->key()) {
      return combine_leaf(combine, value, leaf);
    }
    auto new_leaf = combine_new_leaf<IntegerType, Value>(combine, key, value);
    if (new_leaf == nullptr) {
      return leaf;
    }
    return ptmap_impl::join<IntegerType, Value>(
        key, new_leaf, leaf->key(), leaf);
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (match_prefix(key, branch->prefix(), branch->branching_bit())) {
    if (is_zero_bit(key, branch->branching_bit())) {
      auto new_left_tree = update(combine, key, value, branch->left_tree());
      if (new_left_tree == branch->left_tree()) {
        return branch;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         new_left_tree,
                         branch->right_tree());
    } else {
      auto new_right_tree = update(combine, key, value, branch->right_tree());
      if (new_right_tree == branch->right_tree()) {
        return branch;
      }
      return make_branch(branch->prefix(),
                         branch->branching_bit(),
                         branch->left_tree(),
                         new_right_tree);
    }
  }
  auto new_leaf = combine_new_leaf<IntegerType, Value>(combine, key, value);
  if (new_leaf == nullptr) {
    return branch;
  }
  return ptmap_impl::join<IntegerType, Value>(
      key, new_leaf, branch->prefix(), branch);
}

// Maps all entries with non-default values, applying a given function.
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> map(
    const MappingFunction<typename Value::type>& f,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    auto new_value = f(leaf->value());
    return combine_leaf(ptmap_impl::snd<typename Value::type>, new_value, leaf);
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  auto new_left_tree = map(f, branch->left_tree());
  auto new_right_tree = map(f, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return branch;
  }
  return make_branch(
      branch->prefix(), branch->branching_bit(), new_left_tree, new_right_tree);
}

// Erases all entries where keys and :key_mask share common bits.
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>
erase_all_matching(
    IntegerType key_mask,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree) {
  if (tree == nullptr) {
    return nullptr;
  }
  if (tree->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(tree);
    if (key_mask & leaf->key()) {
      return nullptr;
    }
    return tree;
  }
  const auto& branch =
      boost::static_pointer_cast<PatriciaTreeBranch<IntegerType, Value>>(tree);
  if (key_mask & branch->prefix()) {
    return nullptr;
  }
  if (key_mask < branch->branching_bit()) {
    return branch;
  }
  auto new_left_tree = erase_all_matching(key_mask, branch->left_tree());
  auto new_right_tree = erase_all_matching(key_mask, branch->right_tree());
  if (new_left_tree == branch->left_tree() &&
      new_right_tree == branch->right_tree()) {
    return branch;
  }
  return make_branch(
      branch->prefix(), branch->branching_bit(), new_left_tree, new_right_tree);
}

// We keep the notations of the paper so as to make the implementation easier
// to follow.
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> merge(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t) {
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
  if (s->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    return update(combine, leaf->key(), leaf->value(), t);
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    return update(combine, leaf->key(), leaf->value(), s);
  }
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
    // The two trees have the same prefix. We just merge the subtrees.
    auto new_left = merge(combine, s0, t0);
    auto new_right = merge(combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    }
    if (new_left == t0 && new_right == t1) {
      return t;
    }
    return PatriciaTreeBranch<IntegerType, Value>::make(
        p, m, new_left, new_right);
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Merge t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = merge(combine, s0, t);
      if (s0 == new_left) {
        return s;
      }
      return PatriciaTreeBranch<IntegerType, Value>::make(p, m, new_left, s1);
    } else {
      auto new_right = merge(combine, s1, t);
      if (s1 == new_right) {
        return s;
      }
      return PatriciaTreeBranch<IntegerType, Value>::make(p, m, s0, new_right);
    }
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Merge s with a subtree of t.
    if (is_zero_bit(p, n)) {
      auto new_left = merge(combine, s, t0);
      if (t0 == new_left) {
        return t;
      }
      return PatriciaTreeBranch<IntegerType, Value>::make(q, n, new_left, t1);
    } else {
      auto new_right = merge(combine, s, t1);
      if (t1 == new_right) {
        return t;
      }
      return PatriciaTreeBranch<IntegerType, Value>::make(q, n, t0, new_right);
    }
  }
  // The prefixes disagree.
  return ptmap_impl::join(p, s, q, t);
}

// Combine :value with the value in :leaf with combine(:leaf, :value).
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> combine_leaf(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const typename Value::type& value,
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>>& leaf) {
  auto combined_value = combine(leaf->value(), value);
  if (Value::is_default_value(combined_value)) {
    return nullptr;
  }
  if (!Value::equals(combined_value, leaf->value())) {
    return PatriciaTreeLeaf<IntegerType, Value>::make(leaf->key(),
                                                      combined_value);
  }
  return leaf;
}

// Create a new leaf with the default value and combine :value into it.
template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> combine_new_leaf(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    IntegerType key,
    const typename Value::type& value) {
  auto new_leaf = boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>>(
      PatriciaTreeLeaf<IntegerType, Value>::make(key, Value::default_value()));
  return combine_leaf(combine, value, new_leaf);
}

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> intersect(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t) {
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
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    auto* value = find_value(leaf->key(), t);
    if (value == nullptr) {
      return nullptr;
    }
    return combine_leaf(combine, *value, leaf);
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    auto* value = find_value(leaf->key(), s);
    if (value == nullptr) {
      return nullptr;
    }
    return combine_leaf(combine, *value, leaf);
  }
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
    // The two trees have the same prefix. We merge the intersection of the
    // corresponding subtrees.
    //
    // The subtrees don't have overlapping explicit values, but the combining
    // function will still be called to merge the elements in one tree with the
    // implicit default values in the other.
    return merge<IntegerType, Value>(
        [](const typename Value::type& x, const typename Value::type& y) ->
        typename Value::type {
          if (Value::is_default_value(x)) {
            return y;
          }
          if (Value::is_default_value(y)) {
            return x;
          }
          BOOST_THROW_EXCEPTION(internal_error()
                                << error_msg("Malformed Patricia tree"));
        },
        intersect(combine, s0, t0),
        intersect(combine, s1, t1));
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Intersect t with a subtree of s.
    return intersect(combine, is_zero_bit(q, m) ? s0 : s1, t);
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Intersect s with a subtree of t.
    return intersect(combine, s, is_zero_bit(p, n) ? t0 : t1);
  }
  // The prefixes disagree.
  return nullptr;
}

template <typename IntegerType, typename Value>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> diff(
    const ptmap_impl::CombiningFunction<typename Value::type>& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t) {
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
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    auto* value = find_value(leaf->key(), t);
    if (value == nullptr) {
      return s;
    }
    return combine_leaf(combine, *value, leaf);
  }
  if (t->is_leaf()) {
    const auto& leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    return update(combine, leaf->key(), leaf->value(), s);
  }
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
  auto combine_separate_trees = [](const typename Value::type& x,
                                   const typename Value::type& y) ->
      typename Value::type {
        if (Value::is_default_value(x)) {
          return y;
        }
        if (Value::is_default_value(y)) {
          return x;
        }
        BOOST_THROW_EXCEPTION(internal_error()
                              << error_msg("Malformed Patricia tree"));
      };
  if (m == n && p == q) {
    // The two trees have the same prefix. We merge the difference of the
    // corresponding subtrees.
    auto new_left = diff(combine, s0, t0);
    auto new_right = diff(combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    }
    return merge<IntegerType, Value>(
        combine_separate_trees, new_left, new_right);
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Diff t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = diff(combine, s0, t);
      if (new_left == s0) {
        return s;
      }
      return merge<IntegerType, Value>(combine_separate_trees, new_left, s1);
    } else {
      auto new_right = diff(combine, s1, t);
      if (new_right == s1) {
        return s;
      }
      return merge<IntegerType, Value>(combine_separate_trees, s0, new_right);
    }
  }
  if (m > n && match_prefix(p, q, n)) {
    // p contains q. Diff s with a subtree of t.
    if (is_zero_bit(p, n)) {
      return diff(combine, s, t0);
    } else {
      return diff(combine, s, t1);
    }
  }
  // The prefixes disagree.
  return s;
}

// The iterator basically performs a post-order traversal of the tree, pausing
// at each leaf.
template <typename Key, typename Value>
class PatriciaTreeIterator final {
 public:
  // C++ iterator concept member types
  using iterator_category = std::forward_iterator_tag;
  using mapped_type = typename Value::type;
  using value_type = std::pair<Key, mapped_type>;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = const value_type&;

  using IntegerType =
      typename std::conditional_t<std::is_pointer<Key>::value, uintptr_t, Key>;

  PatriciaTreeIterator() {}

  explicit PatriciaTreeIterator(
      boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> tree)
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

  const std::pair<Key, mapped_type>& operator*() const {
    return *reinterpret_cast<const std::pair<Key, mapped_type>*>(
        &m_leaf->m_pair);
  }

  const std::pair<Key, mapped_type>* operator->() const {
    return reinterpret_cast<const std::pair<Key, mapped_type>*>(
        &m_leaf->m_pair);
  }

 private:
  // The argument is never null.
  void go_to_next_leaf(
      const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& tree) {
    auto* t = tree.get();
    // We go to the leftmost leaf, storing the branches that we're traversing
    // on the stack. By definition of a Patricia tree, a branch node always
    // has two children, hence the leftmost leaf always exists.
    while (t->is_branch()) {
      auto branch = static_cast<PatriciaTreeBranch<IntegerType, Value>*>(t);
      m_stack.push(branch);
      t = branch->left_tree().get();
      // A branch node always has two children.
      RUNTIME_CHECK(t != nullptr, internal_error());
    }
    m_leaf = static_cast<PatriciaTreeLeaf<IntegerType, Value>*>(t);
  }

  // We are holding on to the root of the tree to ensure that all its nested
  // branches and leaves stay alive for as long as the iterator stays alive.
  boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> m_root;
  std::stack<PatriciaTreeBranch<IntegerType, Value>*> m_stack;
  PatriciaTreeLeaf<IntegerType, Value>* m_leaf{nullptr};
};

} // namespace ptmap_impl

} // namespace sparta
