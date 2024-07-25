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

#include <sparta/AbstractMap.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/Exceptions.h>
#include <sparta/PatriciaTreeCore.h>
#include <sparta/PatriciaTreeUtil.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {
namespace ptm_impl {
template <typename Key, typename Value, typename ValueInterface>
class PatriciaTreeMapStaticAssert;
}

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
 * ValueInterface is a structure that must inherit from `AbstractMapValue`. It
 * should contain the following components:
 *
 *   struct ValueInterface {
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
 *
 *     // Whether the default value is top, bottom, or an arbitrary value.
 *     constexpr static AbstractValueKind default_value_kind;
 *   }
 *
 * Patricia trees can only handle unsigned integers. Arbitrary objects can be
 * accommodated as long as they are represented as pointers. Our implementation
 * of Patricia-tree maps can transparently operate on keys that are either
 * unsigned integers or pointers to objects.
 */
template <typename Key,
          typename Value,
          typename ValueInterface = pt_core::SimpleValue<Value>>
class PatriciaTreeMap final
    : public AbstractMap<PatriciaTreeMap<Key, Value, ValueInterface>>,
      private ptm_impl::
          PatriciaTreeMapStaticAssert<Key, Value, ValueInterface> {
  using Core = pt_core::PatriciaTreeCore<Key, ValueInterface>;
  using Codec = typename Core::Codec;

 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename Core::ValueType;
  using value_type = std::pair<Key, mapped_type>;
  using iterator = typename Core::IteratorType;
  using const_iterator = iterator;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const value_type&;
  using const_pointer = const value_type*;

  using IntegerType = typename Codec::IntegerType;
  using value_interface = ValueInterface;
  constexpr static AbstractMapMutability mutability =
      AbstractMapMutability::Immutable;

  bool empty() const { return m_core.empty(); }

  size_t size() const { return m_core.size(); }

  size_t max_size() const { return m_core.max_size(); }

  iterator begin() const { return m_core.begin(); }

  iterator end() const { return m_core.end(); }

  const mapped_type& at(Key key) const { return m_core.at(key); }

  bool leq(const PatriciaTreeMap& other) const {
    return m_core.leq(other.m_core,
                      [](auto, const mapped_type& a, const mapped_type& b) {
                        return ValueInterface::leq(a, b);
                      });
  }

  bool equals(const PatriciaTreeMap& other) const {
    return m_core.equals(other.m_core);
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
    m_core.update(apply_leafs(std::forward<Operation>(operation)), key);
    return *this;
  }

  template <typename MappingFunction> // mapped_type(const mapped_type&)
  bool transform(MappingFunction&& f) {
    return m_core.update_all_leafs(
        apply_leafs(std::forward<MappingFunction>(f)));
  }

  /*
   * Visit all key-value pairs.
   * This does NOT allocate memory, unlike the iterators.
   */
  template <typename Visitor> // void(const value_type&)
  void visit(Visitor&& visitor) const {
    m_core.visit_all_leafs(std::forward<Visitor>(visitor));
  }

  PatriciaTreeMap& remove(Key key) {
    m_core.remove(key);
    return *this;
  }

  template <typename Predicate> // bool(const Key&, const mapped_type&)
  PatriciaTreeMap& filter(Predicate&& predicate) {
    m_core.filter(std::forward<Predicate>(predicate));
    return *this;
  }

  bool erase_all_matching(Key key_mask) {
    return m_core.erase_all_matching(key_mask);
  }

  // Requires CombiningFunction to coerce to
  // std::function<mapped_type(const mapped_type&, const mapped_type&)>
  template <typename CombiningFunction>
  PatriciaTreeMap& union_with(CombiningFunction&& combine,
                              const PatriciaTreeMap& other) {
    m_core.merge(apply_leafs(std::forward<CombiningFunction>(combine)),
                 other.m_core);
    return *this;
  }

  template <typename CombiningFunction>
  PatriciaTreeMap& intersection_with(CombiningFunction&& combine,
                                     const PatriciaTreeMap& other) {
    m_core.intersect(apply_leafs(std::forward<CombiningFunction>(combine)),
                     other.m_core);
    return *this;
  }

  // Requires that `combine(bottom, ...) = bottom`.
  template <typename CombiningFunction>
  PatriciaTreeMap& difference_with(CombiningFunction&& combine,
                                   const PatriciaTreeMap& other) {
    m_core.diff(apply_leafs(std::forward<CombiningFunction>(combine)),
                other.m_core);
    return *this;
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
    return [func = fwd_capture(std::forward<Func>(func))](
               const auto&... leaf_ptrs) mutable {
      auto default_value = ValueInterface::default_value();
      auto return_value =
          func.get()((leaf_ptrs ? leaf_ptrs->value() : default_value)...);

      return keep_if_non_default(std::move(return_value));
    };
  }

  inline static boost::optional<mapped_type> keep_if_non_default(
      mapped_type value) {
    if (ValueInterface::is_default_value(value)) {
      return boost::none;
    } else {
      return value;
    }
  }

  Core m_core;
};

namespace ptm_impl {
template <typename Key, typename Value, typename ValueInterface>
class PatriciaTreeMapStaticAssert {
 protected:
  ~PatriciaTreeMapStaticAssert() {
    static_assert(
        std::is_same_v<
            Value,
            typename pt_core::PatriciaTreeCore<Key, ValueInterface>::ValueType>,
        "Value must be equal to ValueInterface::type");
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    ValueInterface::check_interface();
  }
};
} // namespace ptm_impl

} // namespace sparta
