/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>
#include <utility>

namespace sparta {

enum class AbstractMapMutability {
  // The abstract map is immutable.
  // Unary operators will have signature `Domain(const Domain&)`
  // Binary operators will have signature `Domain(const Domain&, const Domain&)`
  Immutable,

  // The abstract map is mutable.
  // Unary operators will have signature `void(Domain*)`
  // Binary operators will have signature `void(Domain*, const Domain&)`
  Mutable,
};

/*
 * This describes the API for a generic map container.
 */
template <typename Derived>
class AbstractMap {
 public:
  ~AbstractMap() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_base_of<AbstractMap<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractMap");
    static_assert(std::is_final<Derived>::value, "Derived is not final");

    using Key = typename Derived::key_type;
    using Value = typename Derived::mapped_type;
    using KeyValuePair = typename Derived::value_type;
    using Iterator = typename Derived::iterator;

    // Derived();
    static_assert(std::is_default_constructible<Derived>::value,
                  "Derived is not default constructible");

    // Derived(const Derived&);
    static_assert(std::is_copy_constructible<Derived>::value,
                  "Derived is not copy constructible");

    // Derived& operator=(const Derived&);
    static_assert(std::is_copy_assignable<Derived>::value,
                  "Derived is not copy assignable");

    // constexpr static AbstractMapMutability mutability;
    static_assert(std::is_same<decltype(Derived::mutability),
                               const AbstractMapMutability>::value,
                  "Derived::mutability does not exist");

    // bool empty() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().empty()),
                               bool>::value,
                  "Derived::empty() does not exist");

    // std::size_t size() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().size()),
                               std::size_t>::value,
                  "Derived::size() does not exist");

    // std::size_t max_size() const;
    static_assert(
        std::is_same<decltype(std::declval<const Derived>().max_size()),
                     std::size_t>::value,
        "Derived::max_size() does not exist");

    // iterator begin() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().begin()),
                               Iterator>::value,
                  "Derived::begin() does not exist");

    // iterator end() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().end()),
                               Iterator>::value,
                  "Derived::begin() does not exist");

    // const Value& at(const Key&) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().at(
                                   std::declval<const Key>())),
                               const Value&>::value,
                  "Derived::at(const Key&) does not exist");

    // Derived& insert_or_assign(const Key& key, Value value);
    static_assert(
        std::is_same<decltype(std::declval<Derived>().insert_or_assign(
                         std::declval<const Key>(), std::declval<Value>())),
                     Derived&>::value,
        "Derived::insert_or_assign(const Key&, Value) does not exist");

    // Derived& remove(const Key& key);
    static_assert(std::is_same<decltype(std::declval<Derived>().remove(
                                   std::declval<const Key>())),
                               Derived&>::value,
                  "Derived::remove(const Key&) does not exist");

    // void clear();
    static_assert(std::is_void_v<decltype(std::declval<Derived>().clear())>,
                  "Derived::clear() does not exist");

    // void visit(Visitor&& visitor) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().visit(
                                   std::declval<void(const KeyValuePair&)>())),
                               void>::value,
                  "Derived::visit(Visitor&&) does not exist");

    // Derived& filter(Predicate&& predicate);
    static_assert(
        std::is_same<decltype(std::declval<Derived>().filter(
                         std::declval<bool(const Key&, const Value&)>())),
                     Derived&>::value,
        "Derived::filter(Predicate&&) does not exist");

    /*
     * Erase all keys matching with the given pattern, i.e `key & pattern != 0`.
     * This is only implemented by patricia trees.
     */
    // bool erase_all_matching(const Key& key);
    static_assert(
        std::is_same<decltype(std::declval<Derived>().erase_all_matching(
                         std::declval<const Key>())),
                     bool>::value,
        "Derived::erase_all_matching(const Key&) does not exist");

    /*
     * The partial order relation.
     */
    // bool leq(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().leq(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::leq(const Derived&) does not exist");

    /*
     * a.equals(b) is semantically equivalent to a.leq(b) && b.leq(a).
     */
    // bool equals(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().equals(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::equals(const Derived&) does not exist");

    if constexpr (Derived::mutability == AbstractMapMutability::Immutable) {
      // Derived& update(Operation&& operation, const Key& key);
      static_assert(std::is_same<decltype(std::declval<Derived>().update(
                                     std::declval<Value(const Value&)>(),
                                     std::declval<const Key>())),
                                 Derived&>::value,
                    "Derived::update(Operation&&, const Key&) does not exist");

      // bool transform(MappingFunction&& f);
      static_assert(std::is_same<decltype(std::declval<Derived>().transform(
                                     std::declval<Value(const Value&)>())),
                                 bool>::value,
                    "Derived::transform(MappingFunction&&) does not exist");

      // Derived& union_with(CombiningFunction&& combine, const Derived& other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().union_with(
                           std::declval<Value(const Value&, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::union_with(CombiningFunction&&, const Derived&) does not "
          "exist");

      // Derived& intersection_with(CombiningFunction&& combine, const Derived&
      // other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().intersection_with(
                           std::declval<Value(const Value&, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::intersection_with(CombiningFunction&&, const Derived&) "
          "does "
          "not exist");

      // Derived& difference_with(CombiningFunction&& combine, const Derived&
      // other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().difference_with(
                           std::declval<Value(const Value&, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::difference_with(CombiningFunction&&, const Derived&) does "
          "not exist");
    } else if constexpr (Derived::mutability ==
                         AbstractMapMutability::Mutable) {
      // Derived& update(Operation&& operation, const Key& key);
      static_assert(std::is_same<decltype(std::declval<Derived>().update(
                                     std::declval<void(Value*)>(),
                                     std::declval<const Key>())),
                                 Derived&>::value,
                    "Derived::update(Operation&&, const Key&) does not exist");

      // void transform(MappingFunction&& f);
      static_assert(std::is_same<decltype(std::declval<Derived>().transform(
                                     std::declval<void(Value*)>())),
                                 void>::value,
                    "Derived::transform(MappingFunction&&) does not exist");

      // Derived& union_with(CombiningFunction&& combine, const Derived& other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().union_with(
                           std::declval<void(Value*, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::union_with(CombiningFunction&&, const Derived&) does not "
          "exist");

      // Derived& intersection_with(CombiningFunction&& combine, const Derived&
      // other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().intersection_with(
                           std::declval<void(Value*, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::intersection_with(CombiningFunction&&, const Derived&) "
          "does not exist");

      // Derived& difference_with(CombiningFunction&& combine, const Derived&
      // other);
      static_assert(
          std::is_same<decltype(std::declval<Derived>().difference_with(
                           std::declval<void(Value*, const Value&)>(),
                           std::declval<const Derived>())),
                       Derived&>::value,
          "Derived::difference_with(CombiningFunction&&, const Derived&) does "
          "not exist");
    }
  }

  /*
   * Many C++ libraries default to using operator== to check for equality,
   * so we define it here as an alias of equals().
   */
  friend bool operator==(const Derived& self, const Derived& other) {
    return self.equals(other);
  }

  friend bool operator!=(const Derived& self, const Derived& other) {
    return !self.equals(other);
  }

  template <typename CombiningFunction>
  Derived get_union_with(CombiningFunction&& combine,
                         const Derived& other) const {
    // Here and below: the static_cast is required in order to instruct
    // the compiler to use the copy constructor of the derived class.
    Derived result(static_cast<const Derived&>(*this));
    result.union_with(std::forward<CombiningFunction>(combine), other);
    return result;
  }

  template <typename CombiningFunction>
  Derived get_intersection_with(CombiningFunction&& combine,
                                const Derived& other) const {
    Derived result(static_cast<const Derived&>(*this));
    result.intersection_with(std::forward<CombiningFunction>(combine), other);
    return result;
  }

  template <typename CombiningFunction>
  Derived get_difference_with(CombiningFunction&& combine,
                              const Derived& other) const {
    Derived result(static_cast<const Derived&>(*this));
    result.difference_with(std::forward<CombiningFunction>(combine), other);
    return result;
  }
};

} // namespace sparta
