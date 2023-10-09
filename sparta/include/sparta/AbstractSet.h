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

/*
 * This describes the API for a generic set container.
 */
template <typename Derived>
class AbstractSet {
 public:
  ~AbstractSet() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_base_of<AbstractSet<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractSet");
    static_assert(std::is_final<Derived>::value, "Derived is not final");

    using Element = typename Derived::value_type;
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

    // Derived& insert(const Element& element);
    static_assert(std::is_same<decltype(std::declval<Derived>().insert(
                                   std::declval<const Element>())),
                               Derived&>::value,
                  "Derived::insert(const Element&) does not exist");

    // Derived& remove(const Element& element);
    static_assert(std::is_same<decltype(std::declval<Derived>().remove(
                                   std::declval<const Element>())),
                               Derived&>::value,
                  "Derived::remove(const Element&) does not exist");

    // void clear();
    static_assert(std::is_void_v<decltype(std::declval<Derived>().clear())>,
                  "Derived::clear() does not exist");
    /*
     * If the set is a singleton, returns a pointer to the element.
     * Otherwise, returns nullptr.
     */
    // const Element* singleton() const;
    static_assert(
        std::is_same<decltype(std::declval<const Derived>().singleton()),
                     const Element*>::value,
        "Derived::singleton() does not exist");

    // bool contains(const Element& element) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().contains(
                                   std::declval<const Element>())),
                               bool>::value,
                  "Derived::contains(const Element&) does not exist");

    // bool is_subset_of(const Derived& other) const;
    static_assert(
        std::is_same<decltype(std::declval<const Derived>().is_subset_of(
                         std::declval<const Derived>())),
                     bool>::value,
        "Derived::is_subset_of(const Derived&) does not exist");

    // bool equals(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().equals(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::equals(const Derived&) does not exist");

    // void visit(Visitor&& visitor) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().visit(
                                   std::declval<void(const Element&)>())),
                               void>::value,
                  "Derived::visit(Visitor&&) does not exist");

    // Derived& filter(Predicate&& predicate);
    static_assert(std::is_same<decltype(std::declval<Derived>().filter(
                                   std::declval<bool(const Element&)>())),
                               Derived&>::value,
                  "Derived::filter(Predicate&&) does not exist");

    // Derived& union_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().union_with(
                                   std::declval<const Derived>())),
                               Derived&>::value,
                  "Derived::union_with(const Derived&) does not exist");

    // Derived& intersection_with(const Derived& other);
    static_assert(
        std::is_same<decltype(std::declval<Derived>().intersection_with(
                         std::declval<const Derived>())),
                     Derived&>::value,
        "Derived::intersection_with(const Derived&) does not exist");

    // Derived& difference_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().difference_with(
                                   std::declval<const Derived>())),
                               Derived&>::value,
                  "Derived::difference_with(const Derived&) does not exist");
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

  Derived get_union_with(const Derived& other) const {
    Derived result(static_cast<const Derived&>(*this));
    result.union_with(other);
    return result;
  }

  Derived get_intersection_with(const Derived& other) const {
    Derived result(static_cast<const Derived&>(*this));
    result.intersection_with(other);
    return result;
  }

  Derived get_difference_with(const Derived& other) const {
    Derived result(static_cast<const Derived&>(*this));
    result.difference_with(other);
    return result;
  }
};

} // namespace sparta
