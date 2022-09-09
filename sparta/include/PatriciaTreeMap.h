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

  PatriciaTreeMap& insert_or_assign(Key key, mapped_type value) {
    m_core.upsert(key, keep_if_non_default(std::move(value)));
    return *this;
  }

  template <typename Operation> // mapped_type(const mapped_type&)
  PatriciaTreeMap& update(Operation&& operation, Key key) {
    m_core.update(apply_leafs(operation), key);
    return *this;
  }

  template <typename MappingFunction> // mapped_type(const mapped_type&)
  bool map(MappingFunction&& f) {
    return m_core.update_all_leafs(apply_leafs(f));
  }

  PatriciaTreeMap& remove(Key key) {
    m_core.remove(key);
    return *this;
  }

  template <typename Predicate> // bool(const Key&, const ValueType&)
  PatriciaTreeMap& filter(Predicate&& predicate) {
    m_core.filter(predicate);
    return *this;
  }

  bool erase_all_matching(Key key_mask) {
    return m_core.erase_all_matching(key_mask);
  }

  // Requires CombiningFunction to coerce to
  // std::function<mapped_type(const mapped_type&, const mapped_type&)>
  template <typename CombiningFunction>
  PatriciaTreeMap& union_with(const CombiningFunction& combine,
                              const PatriciaTreeMap& other) {
    m_core.merge(apply_leafs(combine), other.m_core);
    return *this;
  }

  template <typename CombiningFunction>
  PatriciaTreeMap& intersection_with(const CombiningFunction& combine,
                                     const PatriciaTreeMap& other) {
    m_core.intersect(apply_leafs(combine), other.m_core);
    return *this;
  }

  // Requires that `combine(bottom, ...) = bottom`.
  template <typename CombiningFunction>
  PatriciaTreeMap& difference_with(const CombiningFunction& combine,
                                   const PatriciaTreeMap& other) {
    m_core.diff(apply_leafs(combine), other.m_core);
    return *this;
  }

  template <typename CombiningFunction>
  PatriciaTreeMap get_union_with(const CombiningFunction& combine,
                                 const PatriciaTreeMap& other) const {
    auto result = *this;
    result.union_with(combine, other);
    return result;
  }

  template <typename CombiningFunction>
  PatriciaTreeMap get_intersection_with(const CombiningFunction& combine,
                                        const PatriciaTreeMap& other) const {
    auto result = *this;
    result.intersection_with(combine, other);
    return result;
  }

  template <typename CombiningFunction>
  PatriciaTreeMap get_difference_with(const CombiningFunction& combine,
                                      const PatriciaTreeMap& other) const {
    auto result = *this;
    result.difference_with(combine, other);
    return result;
  }

  void clear() { m_core.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const PatriciaTreeMap& s) {
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
};

} // namespace sparta
