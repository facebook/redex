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
    static std::tuple<std::function<bool(const Domains&)>...> predicates(
        [](const Domains& component) { return component.is_top(); }...);
    bool are_all_components_top;
    all_of(predicates, &are_all_components_top);
    return are_all_components_top;
  }

  // Since our baseline compiler doesn't support C++14's polymorphic lambdas, we
  // need to define a tuple of lambdas and apply it to the components in order
  // to implement each domain operation.

  bool leq(const Derived& other) const override {
    static std::tuple<std::function<bool(const Domains&, const Domains&)>...>
    predicates(&Domains::leq...);
    bool result;
    compare_with(other.m_product, predicates, &result);
    return result;
  }

  bool equals(const Derived& other) const override {
    static std::tuple<std::function<bool(const Domains&, const Domains&)>...>
    predicates(&Domains::equals...);
    bool result;
    compare_with(other.m_product, predicates, &result);
    return result;
  }

  void set_to_bottom() override {
    static std::tuple<std::function<void(size_t, Domains*)>...> operations([](
        size_t index, Domains* component) { component->set_to_bottom(); }...);
    for_each(operations);
  }

  void set_to_top() override {
    static std::tuple<std::function<void(size_t, Domains*)>...> operations(
        [](size_t index, Domains* component) { component->set_to_top(); }...);
    for_each(operations);
  }

  void join_with(const Derived& other) override {
    static std::tuple<std::function<void(Domains*, const Domains&)>...> joins(
        &Domains::join_with...);
    combine_with(other.m_product, joins, /* smash_bottom */ false);
  }

  void widen_with(const Derived& other) override {
    static std::tuple<std::function<void(Domains*, const Domains&)>...>
    widenings(&Domains::widen_with...);
    combine_with(other.m_product, widenings, /* smash_bottom */ false);
  }

  // We leave the Meet and Narrowing methods virtual, because one might want
  // to refine the result of these operations by applying reduce(). The default
  // implementation doesn't call reduce() as it might be too costly to perform
  // this operation after each Meet, or it might even break the termination
  // property of the Narrowing.

  virtual void meet_with(const Derived& other) override {
    static std::tuple<std::function<void(Domains*, const Domains&)>...> meets(
        &Domains::meet_with...);
    combine_with(other.m_product, meets, /* smash_bottom */ true);
  }

  virtual void narrow_with(const Derived& other) override {
    static std::tuple<std::function<void(Domains*, const Domains&)>...>
    narrowings(&Domains::narrow_with...);
    combine_with(other.m_product, narrowings, /* smash_bottom */ true);
  }

 private:
  // Performs the smash-bottom normalization of a tuple of abstract values.
  void normalize() {
    static std::tuple<std::function<bool(const Domains&)>...> predicates(
        [](const Domains& component) { return component.is_bottom(); }...);
    bool is_one_component_bottom = false;
    any_of(predicates, &is_one_component_bottom);
    if (is_one_component_bottom) {
      set_to_bottom();
    }
  }

  // The following methods are used to unpack a tuple of operations/predicates
  // and apply them to a tuple of abstract values. We use the template deduction
  // mechanism combined with recursion to simulate a sequential iteration over
  // the components ranging from index 0 to sizeof...(Domains) - 1.

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type all_of(
      const std::tuple<std::function<bool(const DomainTypes&)>...>& predicates,
      bool* result) const {
    *result = true;
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type all_of(
      const std::tuple<std::function<bool(const DomainTypes&)>...>& predicates,
      bool* result) const {
    if (!std::get<Index>(predicates)(std::get<Index>(m_product))) {
      *result = false;
      return;
    }
    all_of<Index + 1>(predicates, result);
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type any_of(
      const std::tuple<std::function<bool(const DomainTypes&)>...>& predicates,
      bool* result) const {
    *result = false;
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type any_of(
      const std::tuple<std::function<bool(const DomainTypes&)>...>& predicates,
      bool* result) const {
    if (std::get<Index>(predicates)(std::get<Index>(m_product))) {
      *result = true;
      return;
    }
    all_of<Index + 1>(predicates, result);
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type for_each(
      const std::tuple<std::function<void(size_t, DomainTypes*)>...>&
          operations) {}

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type for_each(
      const std::tuple<std::function<void(size_t, DomainTypes*)>...>&
          operations) {
    std::get<Index>(operations)(Index, &std::get<Index>(m_product));
    for_each<Index + 1>(operations);
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type for_each(
      const std::tuple<std::function<void(size_t, const DomainTypes&)>...>&
          operations) const {}

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type for_each(
      const std::tuple<std::function<void(size_t, const DomainTypes&)>...>&
          operations) const {
    std::get<Index>(operations)(Index, std::get<Index>(m_product));
    for_each<Index + 1>(operations);
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type compare_with(
      const std::tuple<DomainTypes...>& other_product,
      const std::tuple<std::function<bool(const DomainTypes&,
                                          const DomainTypes&)>...>& predicates,
      bool* result) const {
    *result = true;
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type compare_with(
      const std::tuple<DomainTypes...>& other_product,
      const std::tuple<std::function<bool(const DomainTypes&,
                                          const DomainTypes&)>...>& predicates,
      bool* result) const {
    if (!std::get<Index>(predicates)(std::get<Index>(m_product),
                                     std::get<Index>(other_product))) {
      *result = false;
      return;
    }
    compare_with<Index + 1>(other_product, predicates, result);
  }

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index == sizeof...(DomainTypes)>::type combine_with(
      const std::tuple<DomainTypes...>& other_product,
      const std::tuple<
          std::function<void(DomainTypes*, const DomainTypes&)>...>& operations,
      bool smash_bottom) {}

  template <size_t Index = 0, typename... DomainTypes>
  typename std::enable_if<Index != sizeof...(DomainTypes)>::type combine_with(
      const std::tuple<DomainTypes...>& other_product,
      const std::tuple<
          std::function<void(DomainTypes*, const DomainTypes&)>...>& operations,
      bool smash_bottom) {
    auto& component = std::get<Index>(m_product);
    std::get<Index>(operations)(&component, std::get<Index>(other_product));
    if (smash_bottom && component.is_bottom()) {
      set_to_bottom();
      return;
    }
    combine_with<Index + 1>(other_product, operations, smash_bottom);
  }

  std::tuple<Domains...> m_product;

  template <typename T, typename... Ts>
  friend std::ostream& operator<<(
      std::ostream& o, const ReducedProductAbstractDomain<T, Ts...>& p);
};

template <typename Derived, typename... Domains>
std::ostream& operator<<(
    std::ostream& o,
    const ReducedProductAbstractDomain<Derived, Domains...>& p) {
  std::tuple<std::function<void(size_t, const Domains&)>...> operations(
      [&o](size_t index, const Domains& component) {
        o << component;
        if (index < sizeof...(Domains) - 1) {
          // This is not the last component of the tuple.
          o << ", ";
        }
      }...);
  o << "(";
  p.for_each(operations);
  o << ")";
  return o;
}
