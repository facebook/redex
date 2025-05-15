/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <tuple>
#include <type_traits>

#include <sparta/AbstractDomain.h>
#include <sparta/PerfectForwardCapture.h>

// Forward declarations.
namespace sparta {

template <typename DerivedAgain, typename... DomainsAgain>
class ReducedProductAbstractDomain;

} // namespace sparta

namespace sparta {

/*
 * Direct product of abstract domains D1 x ... x Dn consists of tuples of
 * abstract values (v1, ..., vn). Note that the difference between this and the
 * subclass ReducedProductAbstractDomain is the way we handle components that
 * are bottom. DirectProductAbstractDomain doesn't do normalization,
 * meaning that a non-Bottom direct product can contain components that
 * are Bottom.
 *
 * By default the entire product becomes bottom only if all the components are
 * bottom. Similarly, setting the product to top marks every component as top.
 * This class has the component-wise product logic. The subclass
 * ReducedProductAbstractDomain has the additional normalization logic.
 */
template <typename Derived, typename... Domains>
class DirectProductAbstractDomain : public AbstractDomain<Derived> {
 public:
  static_assert(sizeof...(Domains) >= 2,
                "DirectProductAbstractDomain requires at least two parameters");

  /*
   * Defining a public variadic constructor will invariably lead to instances of
   * the "Most Vexing Parse". Passing a tuple of elements as a single argument
   * to the constructor circumvents the issue without sacrificing readability.
   */
  explicit DirectProductAbstractDomain(std::tuple<Domains...> product)
      : m_product(std::move(product)) {}

  /*
   * A default constructor is required.
   */
  DirectProductAbstractDomain()
      : DirectProductAbstractDomain(std::tuple<Domains...>{}) {}

  /*
   * Returns a read-only reference to a component in the tuple.
   */
  template <size_t Index>
  const typename std::tuple_element<Index, std::tuple<Domains...>>::type& get()
      const {
    return std::get<Index>(m_product);
  }

  /*
   * Updates a component of the tuple by applying a user-defined operation.
   */
  template <
      size_t Index,
      typename Fn = std::function<void(
          typename std::tuple_element<Index, std::tuple<Domains...>>::type*)>>
  void apply(const Fn& operation, bool do_reduction = false) {
    if (is_bottom()) {
      return;
    }
    operation(&std::get<Index>(m_product));
  }

  bool is_bottom() const {
    return all_of([](auto&& component) { return component.is_bottom(); });
  }

  bool is_top() const {
    return all_of([](auto&& component) { return component.is_top(); });
  }

  bool leq(const Derived& other_domain) const {
    return compare_with(other_domain, [](auto&& self, auto&& other) {
      return self.leq(other);
    });
  }

  bool equals(const Derived& other_domain) const {
    return compare_with(other_domain, [](auto&& self, auto&& other) {
      return self.equals(other);
    });
  }

  void set_to_bottom() {
    return tuple_apply(
        [](auto&&... c) { discard({(c.set_to_bottom(), 0)...}); }, m_product);
  }

  void set_to_top() {
    return tuple_apply([](auto&&... c) { discard({(c.set_to_top(), 0)...}); },
                       m_product);
  }

  // Note that one might want to refine the result of meet and narrow.

  void meet_with(const Derived& other_domain) {
    combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.meet_with(other); },
        /* smash_bottom */ false);
  }

  void narrow_with(const Derived& other_domain) {
    combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.narrow_with(other); },
        /* smash_bottom */ false);
  }

  // reduce() should only refine (lower) a given component of a product based on
  // the information in the other components. As such, it only makes sense to
  // call reduce() after meet/narrow -- operations which can refine the
  // components of a product. However, we may still need to canonicalize our
  // product after a join/widen, so these methods might be overriden.

  void join_with(const Derived& other_domain) {
    combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.join_with(other); },
        /* smash_bottom */ false);
  }

  void widen_with(const Derived& other_domain) {
    combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.widen_with(other); },
        /* smash_bottom */ false);
  }

  friend std::ostream& operator<<(std::ostream& o, const Derived& p) {
    o << "(";
    tuple_print(o, p.m_product);
    o << ")";
    return o;
  }

 private:
  // When using tuple_apply, we often need to expand a parameter pack in order
  // to perform an operation on each parameter. This is achieved using the
  // expansion of a brace-enclosed initializer {(expr)...}, where expr operates
  // via side effects. Since we don't use the result of the initializer
  // expansion, some compilers may complain. We use this function to explicitly
  // discard the initializer list and silence those compilers.
  template <typename T>
  static void discard(const std::initializer_list<T>&) {}

  // The following methods are used to unpack a tuple of operations/predicates
  // and apply them to a tuple of abstract values. We use an implementation of
  // C++ 17's std::apply to iterate over the elements of a tuple (see
  // http://en.cppreference.com/w/cpp/utility/apply).

  template <class F, class Tuple, std::size_t... I>
  constexpr decltype(auto) static tuple_apply_impl(F&& f,
                                                   Tuple&& t,
                                                   std::index_sequence<I...>) {
    return f(std::get<I>(std::forward<Tuple>(t))...);
  }

  template <class F, class Tuple>
  constexpr decltype(auto) static tuple_apply(F&& f, Tuple&& t) {
    return tuple_apply_impl(
        std::forward<F>(f),
        std::forward<Tuple>(t),
        std::make_index_sequence<std::tuple_size<std::decay_t<Tuple>>{}>{});
  }

  template <class Predicate>
  bool all_of(Predicate&& predicate) const {
    return tuple_apply(
        [predicate = fwd_capture(std::forward<Predicate>(predicate))](
            const Domains&... component) mutable {
          bool result = true;
          discard({(result &= predicate.get()(component))...});
          return result;
        },
        m_product);
  }

  template <class Predicate>
  bool any_of(Predicate&& predicate) const {
    return tuple_apply(
        [predicate = fwd_capture(std::forward<Predicate>(predicate))](
            const Domains&... component) mutable {
          bool result = false;
          discard({(result |= predicate.get()(component))...});
          return result;
        },
        m_product);
  }

  // We also use the template deduction mechanism combined with recursion to
  // simulate a sequential iteration over the components ranging from index 0 to
  // sizeof...(Domains) - 1.

  template <size_t Index = 0, class Predicate>
  std::enable_if_t<Index == sizeof...(Domains), bool> compare_with(
      const Derived& other, Predicate&& predicate) const {
    return true;
  }

  template <size_t Index = 0, class Predicate>
  std::enable_if_t<Index != sizeof...(Domains), bool> compare_with(
      const Derived& other, Predicate&& predicate) const {
    if (!predicate(std::get<Index>(m_product),
                   std::get<Index>(other.m_product))) {
      return false;
    }
    return compare_with<Index + 1>(other, predicate);
  }

  template <size_t Index = 0, class Operation>
  std::enable_if_t<Index == sizeof...(Domains)> combine_with(
      const Derived& other, Operation&& operation, bool smash_bottom) {}

  template <size_t Index = 0, class Operation>
  std::enable_if_t<Index != sizeof...(Domains)> combine_with(
      const Derived& other, Operation&& operation, bool smash_bottom) {
    auto& component = std::get<Index>(m_product);
    operation(component, std::get<Index>(other.m_product));
    if (smash_bottom && component.is_bottom()) {
      set_to_bottom();
      return;
    }
    combine_with<Index + 1>(other, operation, smash_bottom);
  }

  template <class Tuple, std::size_t... I>
  static void tuple_print_impl(std::ostream& o,
                               Tuple&& t,
                               std::index_sequence<I...>) {
    discard({0, (void(o << (I == 0 ? "" : ", ") << std::get<I>(t)), 0)...});
  }

  template <class Tuple>
  static void tuple_print(std::ostream& o, Tuple&& t) {
    return tuple_print_impl(
        o,
        std::forward<Tuple>(t),
        std::make_index_sequence<std::tuple_size<std::decay_t<Tuple>>{}>{});
  }

  std::tuple<Domains...> m_product;

  template <typename DerivedAgain, typename... DomainsAgain>
  friend class ReducedProductAbstractDomain;
};

} // namespace sparta
