/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <tuple>
#include <type_traits>

#include "AbstractDomain.h"
#include "Debug.h"
#include "Util.h"

/*
 * The reduced cartesian product of abstract domains D1 x ... x Dn consists of
 * tuples of abstract values (v1, ..., vn) that represent the intersection of
 * the denotations of v1, ..., vn. Hence, all tuples that have at least one _|_
 * component are equated to _|_ (this is similar to abstract environments). More
 * complex reduction steps able to infer the emptiness of the intersection when
 * no component is equal to _|_ can be implemented by overriding the reduce()
 * method.
 *
 * The interface uses the curiously recurring template pattern, thus allowing
 * the product domain to define additional operations. This comes handy when
 * each component domain defines a common operation that has to be lifted to the
 * product.
 *
 * Example usage:
 *
 *  class D0 final : public AbstractDomain<D0> {
 *   public:
 *    D0 operation1(...) { ... }
 *    void operation2(...) { ... }
 *    ...
 *  };
 *
 *  class D1 final : public AbstractDomain<D1> {
 *   public:
 *    D1 operation1(...) { ... }
 *    void operation2(...) { ... }
 *    ...
 *  };
 *
 *  class D0xD1 final : public ProductAbstractDomain<D0xD1, D0, D1> {
 *   public:
 *    D0xD1 operation1(...) { ... }
 *    void operation2(...) {
 *      D0& x = get<0>();
 *      D1& y = get<1>();
 *      x.operation2();
 *      y.operation2();
 *      ...
 *    }
 *    static D0xD1 bottom() {
 *      D0xD1 x;
 *      x.set_to_bottom();
 *      return x;
 *    }
 *    static D0xD1 top() {
 *      D0xD1 x;
 *      x.set_to_top();
 *      return x;
 *    }
 *    ...
 *  };
 *
 */
template <typename Derived, typename... Domains>
class ReducedProductAbstractDomain : public AbstractDomain<Derived> {
 public:
  ~ReducedProductAbstractDomain() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(
        sizeof...(Domains) >= 2,
        "ReducedProductAbstractDomain requires at least two parameters");
  }

  ReducedProductAbstractDomain() = default;

  /*
   * Defining a public variadic constructor will invariably lead to instances of
   * the "Most Vexing Parse". Passing a tuple of elements as a single argument
   * to the constructor circumvents the issue without sacrificing readability.
   */
  explicit ReducedProductAbstractDomain(std::tuple<Domains...> product)
      : m_product(product) {
    // Since one or more components can be _|_, we need to normalize the
    // representation.
    normalize();
    // Even though no component is _|_, the intersection of the denotations of
    // the components might still be empty. Deciding the emptiness of the
    // intersection usually involves more sophisticated techniques that are
    // specific to the abstract domains in the product. This step is performed
    // by the reduce() method.
    reduce();
  }

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
   * Since the reduction may involve costly computations and is not always
   * required depending on the nature of the operation performed, we leave it as
   * an optional step.
   */
  template <size_t Index>
  void apply(
      std::function<void(
          typename std::tuple_element<Index, std::tuple<Domains...>>::type*)>
          operation,
      bool do_reduction = false) {
    if (is_bottom()) {
      return;
    }
    operation(&std::get<Index>(m_product));
    if (get<Index>().is_bottom()) {
      set_to_bottom();
      return;
    }
    if (do_reduction) {
      reduce();
    }
  }

  /*
   * The reduction operation of the reduced product (usually denoted by the
   * Greek letter sigma in the literature) is used to decide whether the
   * intersection of the denotations of the components is empty when no
   * component is _|_. This occurs when the component domains have overlapping
   * denotations and can refine each other. For example, one could implement
   * Granger's local iterations to propagate information across components. The
   * reduction operation is specific to the abstract domains used in the product
   * and should be implemented in the derived class.
   */
  virtual void reduce() {}

  bool is_bottom() const override {
    // The normalized _|_ element in the product domain has all its components
    // set to _|_, so that we just need to check the first component.
    return get<0>().is_bottom();
  }

  bool is_top() const override {
    return all_of([](auto&& component) { return component.is_top(); });
  }

  bool leq(const Derived& other_domain) const override {
    return compare_with(other_domain, [](auto&& self, auto&& other) {
      return self.leq(other);
    });
  }

  bool equals(const Derived& other_domain) const override {
    return compare_with(other_domain, [](auto&& self, auto&& other) {
      return self.equals(other);
    });
  }

  void set_to_bottom() override {
    return tuple_apply(
        [](auto&&... c) { int UNUSED expand[] = {(c.set_to_bottom(), 0)...}; },
        m_product);
  }

  void set_to_top() override {
    return tuple_apply(
        [](auto&&... c) { int UNUSED expand[] = {(c.set_to_top(), 0)...}; },
        m_product);
  }

  void join_with(const Derived& other_domain) override {
    combine_with(other_domain,
                 [](auto&& self, auto&& other) { self.join_with(other); },
                 /* smash_bottom */ false);
  }

  void widen_with(const Derived& other_domain) override {
    combine_with(other_domain,
                 [](auto&& self, auto&& other) { self.widen_with(other); },
                 /* smash_bottom */ false);
  }

  // We leave the Meet and Narrowing methods virtual, because one might want
  // to refine the result of these operations by applying reduce(). The default
  // implementation doesn't call reduce() as it might be too costly to perform
  // this operation after each Meet, or it might even break the termination
  // property of the Narrowing.

  virtual void meet_with(const Derived& other_domain) override {
    combine_with(other_domain,
                 [](auto&& self, auto&& other) { self.meet_with(other); },
                 /* smash_bottom */ true);
  }

  virtual void narrow_with(const Derived& other_domain) override {
    combine_with(other_domain,
                 [](auto&& self, auto&& other) { self.narrow_with(other); },
                 /* smash_bottom */ true);
  }

 private:
  // Performs the smash-bottom normalization of a tuple of abstract values.
  void normalize() {
    if (any_of([](auto&& component) { return component.is_bottom(); })) {
      set_to_bottom();
    }
  }

  // The following methods are used to unpack a tuple of operations/predicates
  // and apply them to a tuple of abstract values. We use an implementation of
  // C++ 17's std::apply to iterate tuple elements.
  // - http://en.cppreference.com/w/cpp/utility/apply

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
        [predicate](const Domains&... component) {
          bool result = true;
          bool UNUSED expand[] = {(result &= predicate(component))...};
          return result;
        },
        m_product);
  }

  template <class Predicate>
  bool any_of(Predicate&& predicate) const {
    return tuple_apply(
        [predicate](const Domains&... component) {
          bool result = false;
          bool UNUSED expand[] = {(result |= predicate(component))...};
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

  std::tuple<Domains...> m_product;

  template <typename T, typename... Ts>
  friend std::ostream& operator<<(
      std::ostream& o, const ReducedProductAbstractDomain<T, Ts...>& p);
};

namespace {
template <class Tuple, std::size_t... I>
void tuple_print_impl(std::ostream& o, Tuple&& t, std::index_sequence<I...>) {
  int UNUSED print[] = {
      0, (void(o << (I == 0 ? "" : ", ") << std::get<I>(t)), 0)...};
}

template <class Tuple>
void tuple_print(std::ostream& o, Tuple&& t) {
  return tuple_print_impl(
      o,
      std::forward<Tuple>(t),
      std::make_index_sequence<std::tuple_size<std::decay_t<Tuple>>{}>{});
}
}

template <typename Derived, typename... Domains>
std::ostream& operator<<(
    std::ostream& o,
    const ReducedProductAbstractDomain<Derived, Domains...>& p) {
  o << "(";
  tuple_print(o, p.m_product);
  o << ")";
  return o;
}
