/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

#include <sparta/AbstractDomain.h>
#include <sparta/TypeTraits.h>

namespace sparta {

namespace {
SPARTA_HAS_STATIC_MEMBER_FUNCTION(leq, has_static_leq_member_function)
}

/*
 * This describes the `ValueInterface` structure required for abstract maps.
 */
template <typename Derived>
class AbstractMapValue {
 public:
  /*
   * Check that `Derived` implements the `AbstractMapValue` interface, using
   * static assertions. This must be called from a method that is instantiated,
   * for instance the destructor of the map.
   */
  constexpr static void check_interface() {
    static_assert(std::is_base_of<AbstractMapValue<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractMapValue");
    static_assert(std::is_final<Derived>::value, "Derived is not final");

    // Derived::type
    using type = typename Derived::type;

    /*
     * Returns the default value.
     */
    // static type default_value();
    static_assert(std::is_same<decltype(Derived::default_value()), type>::value,
                  "Derived::default_value() does not exist");

    /*
     * Tests whether a value is the default value.
     */
    // static bool is_default_value(const type& x);
    static_assert(std::is_same<decltype(Derived::is_default_value(
                                   std::declval<const type>())),
                               bool>::value,
                  "Derived::is_default_value(const type&) does not exist");

    /*
     * The equality predicate for values.
     */
    // static bool equals(const type& x, const type& y);
    static_assert(
        std::is_same<decltype(Derived::equals(std::declval<const type>(),
                                              std::declval<const type>())),
                     bool>::value,
        "Derived::equals(const type&, const type&) does not exist");

    if constexpr (has_static_leq_member_function<Derived>::value) {
      /*
       * A partial order relation over values. In order to use the lifted
       * partial order relation over maps PatriciaTreeMap::leq(), this method
       * must be implemented. Additionally, value::type must be an
       * implementation of an AbstractDomain.
       */
      // static bool leq(const type& x, const type& y);
      static_assert(
          std::is_same<decltype(Derived::leq(std::declval<const type>(),
                                             std::declval<const type>())),
                       bool>::value,
          "Derived::leq(const type&, const type&) does not exist");
    }

    /*
     * Whether the default value is top, bottom, or an arbitrary value.
     */
    // constexpr static AbstractValueKind default_value_kind;
    static_assert(std::is_same<decltype(Derived::default_value_kind),
                               const AbstractValueKind>::value,
                  "Derived::default_value_kind does not exist");
  }
};

} // namespace sparta
