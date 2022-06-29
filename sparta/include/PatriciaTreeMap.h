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
#include <ostream>
#include <type_traits>
#include <utility>

#include <boost/optional.hpp>

#include "Exceptions.h"
#include "PatriciaTreeCore.h"
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
using PatriciaTree = pt_core::PatriciaTreeNode<IntegerType, Value>;

template <typename IntegerType, typename Value>
using PatriciaTreeLeaf = pt_core::PatriciaTreeLeaf<IntegerType, Value>;

template <typename IntegerType, typename Value>
using PatriciaTreeBranch = pt_core::PatriciaTreeBranch<IntegerType, Value>;

template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> merge(
    const Combine& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> intersect(
    const Combine& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> diff(
    const Combine& combine,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& s,
    const boost::intrusive_ptr<PatriciaTree<IntegerType, Value>>& t);

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
          typename Value = pt_core::SimpleValue<ValueType>>
class PatriciaTreeMap final {
  using Core = pt_core::PatriciaTreeCore<Key, Value>;
  using Codec = typename Core::Codec;

 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename Core::ValueType;
  using value_type = std::pair<const Key, mapped_type>;
  using iterator = typename Core::IteratorType;
  using const_iterator = iterator;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const mapped_type&;
  using const_pointer = const mapped_type*;

  using IntegerType = typename Codec::IntegerType;
  using combining_function =
      std::function<mapped_type(const mapped_type&, const mapped_type&)>;
  using mapping_function = std::function<mapped_type(const mapped_type&)>;

  static_assert(std::is_same_v<ValueType, mapped_type>,
                "ValueType must be equal to Value::type");

  bool empty() const { return m_core.empty(); }

  size_t size() const { return m_core.size(); }

  size_t max_size() const { return m_core.max_size(); }

  iterator begin() const { return m_core.begin(); }

  iterator end() const { return m_core.end(); }

  const mapped_type& at(Key key) const { return m_core.at(key); }

  bool leq(const PatriciaTreeMap& other) const {
    return m_core.leq(other.m_core);
  }

  bool equals(const PatriciaTreeMap& other) const {
    return m_core.equals(other.m_core);
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
    return m_core.reference_equals(other.m_core);
  }

  PatriciaTreeMap& update(
      const std::function<mapped_type(const mapped_type&)>& operation,
      Key key) {
    m_core.update(apply_leafs(operation), key);
    return *this;
  }

  bool map(const mapping_function& f) {
    return m_core.update_all_leafs(apply_leafs(f));
  }

  bool erase_all_matching(Key key_mask) {
    return m_core.erase_all_matching(key_mask);
  }

  PatriciaTreeMap& insert_or_assign(Key key, mapped_type value) {
    m_core.upsert(key, keep_if_non_default(std::move(value)));
    return *this;
  }

  PatriciaTreeMap& union_with(const combining_function& combine,
                              const PatriciaTreeMap& other) {
    m_core.m_tree = ptmap_impl::merge<IntegerType, Value>(
        apply_leafs(combine), m_core.m_tree, other.m_core.m_tree);
    return *this;
  }

  PatriciaTreeMap& intersection_with(const combining_function& combine,
                                     const PatriciaTreeMap& other) {
    m_core.m_tree = ptmap_impl::intersect<IntegerType, Value>(
        apply_leafs(combine), m_core.m_tree, other.m_core.m_tree);
    return *this;
  }

  // Requires that `combine(bottom, ...) = bottom`.
  PatriciaTreeMap& difference_with(const combining_function& combine,
                                   const PatriciaTreeMap& other) {
    m_core.m_tree = ptmap_impl::diff<IntegerType, Value>(
        apply_leafs(combine), m_core.m_tree, other.m_core.m_tree);
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

  void clear() { m_core.clear(); }

 private:
  // The map implicitly stores default at every node. If a leaf is absent the
  // operation will see a default value instead. Likewise, if the result of an
  // operation is a default value we remove the leaf rather than store it.
  //
  // This wraps the given function to apply these transformations.
  template <typename Func>
  inline static auto apply_leafs(Func&& func) {
    return [func = std::forward<Func>(func)](const auto&... leaf_ptrs) {
      auto default_value = Value::default_value();
      auto return_value =
          func((leaf_ptrs ? leaf_ptrs->value() : default_value)...);

      return keep_if_non_default(std::move(return_value));
    };
  }

  inline static boost::optional<mapped_type> keep_if_non_default(
      mapped_type value) {
    if (Value::is_default_value(value)) {
      return boost::none;
    } else {
      return value;
    }
  }

  Core m_core;

  template <typename T, typename VT, typename V>
  friend std::ostream& ::operator<<(std::ostream&,
                                    const PatriciaTreeMap<T, VT, V>&);
};

} // namespace sparta

template <typename Key, typename ValueType, typename Value>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::PatriciaTreeMap<Key, ValueType, Value>& s) {
  using namespace sparta;
  o << "{";
  for (auto it = s.begin(); it != s.end(); ++it) {
    o << pt_util::deref(it->first) << " -> " << it->second;
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

// We keep the notations of the paper so as to make the implementation easier
// to follow.
template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> merge(
    const Combine& combine,
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
    const auto& s_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    return pt_core::combine_leafs_by_key(combine, s_leaf, s_leaf->key(), t);
  }
  if (t->is_leaf()) {
    const auto& t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    return pt_core::combine_leafs_by_key(combine, t_leaf, t_leaf->key(), s);
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
    return PatriciaTreeBranch<IntegerType, Value>::make(p, m, new_left,
                                                        new_right);
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
  return pt_core::join(p, s, q, t);
}

template <typename IntegerType, typename Value>
boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>> use_available_value(
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>>& x,
    const boost::intrusive_ptr<PatriciaTreeLeaf<IntegerType, Value>>& y) {
  if (x) {
    return x;
  } else if (y) {
    return y;
  } else {
    BOOST_THROW_EXCEPTION(internal_error()
                          << error_msg("Malformed Patricia tree"));
  }
}

template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> intersect(
    const Combine& combine,
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
    const auto& s_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    auto t_leaf = pt_core::find_leaf(s_leaf->key(), t);
    if (t_leaf == nullptr) {
      return nullptr;
    }
    return pt_core::combine_leafs(combine, t_leaf, s_leaf);
  }
  if (t->is_leaf()) {
    const auto& t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    auto s_leaf = pt_core::find_leaf(t_leaf->key(), s);
    if (s_leaf == nullptr) {
      return nullptr;
    }
    return pt_core::combine_leafs(combine, s_leaf, t_leaf);
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
    return merge<IntegerType, Value>(use_available_value<IntegerType, Value>,
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

template <typename IntegerType, typename Value, typename Combine>
inline boost::intrusive_ptr<PatriciaTree<IntegerType, Value>> diff(
    const Combine& combine,
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
    const auto& s_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(s);
    auto t_leaf = pt_core::find_leaf(s_leaf->key(), t);
    if (t_leaf == nullptr) {
      return s;
    }
    return pt_core::combine_leafs(combine, t_leaf, s_leaf);
  }
  if (t->is_leaf()) {
    const auto& t_leaf =
        boost::static_pointer_cast<PatriciaTreeLeaf<IntegerType, Value>>(t);
    return pt_core::combine_leafs_by_key(combine, t_leaf, t_leaf->key(), s);
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
    // The two trees have the same prefix. We merge the difference of the
    // corresponding subtrees.
    auto new_left = diff(combine, s0, t0);
    auto new_right = diff(combine, s1, t1);
    if (new_left == s0 && new_right == s1) {
      return s;
    }
    return merge<IntegerType, Value>(use_available_value<IntegerType, Value>,
                                     new_left, new_right);
  }
  if (m < n && match_prefix(q, p, m)) {
    // q contains p. Diff t with a subtree of s.
    if (is_zero_bit(q, m)) {
      auto new_left = diff(combine, s0, t);
      if (new_left == s0) {
        return s;
      }
      return merge<IntegerType, Value>(use_available_value<IntegerType, Value>,
                                       new_left, s1);
    } else {
      auto new_right = diff(combine, s1, t);
      if (new_right == s1) {
        return s;
      }
      return merge<IntegerType, Value>(use_available_value<IntegerType, Value>,
                                       s0, new_right);
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

} // namespace ptmap_impl

} // namespace sparta
