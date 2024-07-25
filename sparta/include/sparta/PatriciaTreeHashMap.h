/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <ostream>
#include <type_traits>
#include <utility>

#include <boost/container/small_vector.hpp>

#include <sparta/AbstractMap.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/Exceptions.h>
#include <sparta/FlatMap.h>
#include <sparta/FlattenIterator.h>
#include <sparta/PatriciaTreeMap.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {
namespace pthm_impl {
template <typename Key,
          typename Value,
          typename ValueInterface,
          typename KeyHash,
          typename KeyCompare,
          typename KeyEqual>
class PatriciaTreeHashMapStaticAssert;
}

/*
 * This structure implements a generalized hash map on top of patricia trees.
 *
 * This provides most of the benefits of patricia trees (fast merging) for
 * any `Key` type that is hashable and ordered. Prefer using `PatriciaTreeMap`
 * directly if the `Key` type is an integer or pointer.
 *
 * See `PatriciaTreeMap` for more information about patricia trees.
 */
template <typename Key,
          typename Value,
          typename ValueInterface = pt_core::SimpleValue<Value>,
          typename KeyHash = std::hash<Key>,
          typename KeyCompare = std::less<Key>,
          typename KeyEqual = std::equal_to<Key>>
class PatriciaTreeHashMap final
    : public AbstractMap<PatriciaTreeHashMap<Key,
                                             Value,
                                             ValueInterface,
                                             KeyHash,
                                             KeyCompare,
                                             KeyEqual>>,
      private pthm_impl::PatriciaTreeHashMapStaticAssert<Key,
                                                         Value,
                                                         ValueInterface,
                                                         KeyHash,
                                                         KeyCompare,
                                                         KeyEqual> {
 private:
  using SmallVector = boost::container::small_vector<std::pair<Key, Value>, 1>;
  using FlatMapT =
      FlatMap<Key, Value, ValueInterface, KeyCompare, KeyEqual, SmallVector>;

  struct FlatMapValue final : public AbstractMapValue<FlatMapValue> {
    using type = FlatMapT;

    static FlatMapT default_value() { return FlatMapT(); }

    static bool is_default_value(const FlatMapT& map) { return map.empty(); }

    static bool equals(const FlatMapT& a, const FlatMapT& b) {
      return a.equals(b);
    }

    static bool leq(const type& x, const type& y) { return x.leq(y); }

    constexpr static AbstractValueKind default_value_kind =
        ValueInterface::default_value_kind;
  };
  using PatriciaTreeT = PatriciaTreeMap<std::size_t, FlatMapT, FlatMapValue>;

  struct FlattenDereference {
    static typename FlatMapT::iterator begin(
        const std::pair<std::size_t, FlatMapT>& binding) {
      return binding.second.begin();
    }

    static typename FlatMapT::iterator end(
        const std::pair<std::size_t, FlatMapT>& binding) {
      return binding.second.end();
    }
  };
  using FlattenIteratorT = FlattenIterator<typename PatriciaTreeT::iterator,
                                           typename FlatMapT::iterator,
                                           FlattenDereference>;

 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename ValueInterface::type;
  using value_type = typename FlatMapT::value_type;
  using iterator = FlattenIteratorT;
  using const_iterator = FlattenIteratorT;
  using difference_type = typename FlattenIteratorT::difference_type;
  using size_type = size_t;
  using const_reference = typename FlattenIteratorT::reference;
  using const_pointer = typename FlattenIteratorT::pointer;

  using value_interface = ValueInterface;
  constexpr static AbstractMapMutability mutability =
      AbstractMapMutability::Mutable;

  bool empty() const { return m_tree.empty(); }

  size_t size() const {
    size_t s = 0;
    for (const auto& binding : m_tree) {
      s += binding.second.size();
    }
    return s;
  }

  size_t max_size() const { return m_tree.max_size(); }

  iterator begin() const {
    return FlattenIteratorT{m_tree.begin(), m_tree.end()};
  }

  iterator end() const { return FlattenIteratorT{m_tree.end(), m_tree.end()}; }

  const mapped_type& at(const Key& key) const {
    return m_tree.at(KeyHash()(key)).at(key);
  }

  bool leq(const PatriciaTreeHashMap& other) const {
    return m_tree.leq(other.m_tree);
  }

  bool equals(const PatriciaTreeHashMap& other) const {
    return m_tree.equals(other.m_tree);
  }

  /* See `PatriciaTreeMap::reference_equals` */
  bool reference_equals(const PatriciaTreeHashMap& other) const {
    return m_tree.reference_equals(other.m_tree);
  }

  PatriciaTreeHashMap& insert_or_assign(const Key& key, mapped_type value) {
    m_tree.update(
        [value = std::move(value), &key](FlatMapT flat_map) -> FlatMapT {
          flat_map.insert_or_assign(key, std::move(value));
          return flat_map;
        },
        KeyHash()(key));
    return *this;
  }

  template <typename Operation> // void(mapped_type*)
  PatriciaTreeHashMap& update(Operation&& operation, const Key& key) {
    m_tree.update(
        [operation = fwd_capture(std::forward<Operation>(operation)),
         &key](FlatMapT flat_map) mutable -> FlatMapT {
          flat_map.update(std::forward<Operation>(operation.get()), key);
          return flat_map;
        },
        KeyHash()(key));
    return *this;
  }

  template <typename MappingFunction> // void(mapped_type*)
  void transform(MappingFunction&& f) {
    m_tree.transform([f = fwd_capture(std::forward<MappingFunction>(f))](
                         FlatMapT flat_map) mutable -> FlatMapT {
      flat_map.transform(f.get());
      return flat_map;
    });
  }

  /*
   * Visit all key-value pairs.
   * This does NOT allocate memory, unlike the iterators.
   */
  template <typename Visitor> // void(const value_type&)
  void visit(Visitor&& visitor) const {
    m_tree.visit([visitor = fwd_capture(std::forward<Visitor>(visitor))](
                     const std::pair<size_t, FlatMapT>& binding) mutable {
      binding.second.visit(visitor.get());
    });
  }

  PatriciaTreeHashMap& remove(const Key& key) {
    m_tree.update(
        [&key](FlatMapT flat_map) -> FlatMapT {
          flat_map.remove(key);
          return flat_map;
        },
        KeyHash()(key));
    return *this;
  }

  template <typename Predicate> // bool(const Key&, const mapped_type&)
  PatriciaTreeHashMap& filter(Predicate&& predicate) {
    m_tree.transform([predicate = fwd_capture(std::forward<Predicate>(
                          predicate))](FlatMapT flat_map) mutable -> FlatMapT {
      flat_map.filter(predicate.get());
      return flat_map;
    });
    return *this;
  }

  bool erase_all_matching(const Key& key_mask) {
    throw std::logic_error("not implemented");
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  PatriciaTreeHashMap& union_with(CombiningFunction&& combine,
                                  const PatriciaTreeHashMap& other) {
    m_tree.union_with(
        [combine = fwd_capture(std::forward<CombiningFunction>(combine))](
            FlatMapT left, const FlatMapT& right) mutable -> FlatMapT {
          left.union_with(combine.get(), right);
          return left;
        },
        other.m_tree);
    return *this;
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  PatriciaTreeHashMap& intersection_with(CombiningFunction&& combine,
                                         const PatriciaTreeHashMap& other) {
    m_tree.intersection_with(
        [combine = fwd_capture(std::forward<CombiningFunction>(combine))](
            FlatMapT left, const FlatMapT& right) mutable -> FlatMapT {
          left.intersection_with(combine.get(), right);
          return left;
        },
        other.m_tree);
    return *this;
  }

  // Requires that `combine(bottom, ...) = bottom`.
  template <typename CombiningFunction>
  PatriciaTreeHashMap& difference_with(CombiningFunction&& combine,
                                       const PatriciaTreeHashMap& other) {
    m_tree.difference_with(
        [combine = fwd_capture(std::forward<CombiningFunction>(combine))](
            FlatMapT left, const FlatMapT& right) mutable -> FlatMapT {
          left.difference_with(combine.get(), right);
          return left;
        },
        other.m_tree);
    return *this;
  }

  void clear() { m_tree.clear(); }

  friend std::ostream& operator<<(std::ostream& o,
                                  const PatriciaTreeHashMap& s) {
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
  PatriciaTreeT m_tree;
};

namespace pthm_impl {
template <typename Key,
          typename Value,
          typename ValueInterface,
          typename KeyHash,
          typename KeyCompare,
          typename KeyEqual>
class PatriciaTreeHashMapStaticAssert {
 protected:
  ~PatriciaTreeHashMapStaticAssert() {
    static_assert(std::is_same_v<Value, typename ValueInterface::type>,
                  "Value must be equal to ValueInterface::type");
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    ValueInterface::check_interface();
  }
};
} // namespace pthm_impl

} // namespace sparta
