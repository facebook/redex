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

#include <sparta/DirectProductAbstractDomain.h>

namespace sparta {
namespace rpad_impl {
template <typename Derived, typename... Domains>
class ReducedProductAbstractDomainStaticAssert;
} // namespace rpad_impl

/*
 * The reduced cartesian product of abstract domains D1 x ... x Dn consists of
 * tuples of abstract values (v1, ..., vn) that represent the intersection of
 * the denotations of v1, ..., vn. Hence, all tuples that have at least one _|_
 * component are equated to _|_ (this is similar to abstract environments).
 * However, the intersection of the denotations may be empty even though none of
 * the components is equal to _|_.
 *
 * The reduction operation of the reduced product (usually denoted by the
 * Greek letter sigma in the literature) is used to decide whether the
 * intersection of the denotations of the components is empty when no
 * component is _|_. This occurs when the component domains have overlapping
 * denotations and can refine each other. For example, one could implement
 * Granger's local iterations to propagate information across components. The
 * reduction operation is specific to the abstract domains used in the product
 * and should be implemented in the derived class as the `reduce_product()`
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
 *      // Explicitly call the reduction operation
 *      reduce();
 *    }
 *    // The reduce_product method implements the mechanics of the reduction
 *    // operation. It is required by the API, even though it does nothing. The
 *    // reduction operates by changing the contents of the given tuple.
 *    static void reduce_product(std::tuple<D0, D1>& product) { ... }
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
class ReducedProductAbstractDomain
    : public DirectProductAbstractDomain<Derived, Domains...>,
      private rpad_impl::ReducedProductAbstractDomainStaticAssert<Derived,
                                                                  Domains...> {
 public:
  static_assert(
      sizeof...(Domains) >= 2,
      "ReducedProductAbstractDomain requires at least two parameters");

  /*
   * Defining a public variadic constructor will invariably lead to instances of
   * the "Most Vexing Parse". Passing a tuple of elements as a single argument
   * to the constructor circumvents the issue without sacrificing readability.
   */
  explicit ReducedProductAbstractDomain(std::tuple<Domains...> product)
      : DirectProductAbstractDomain<Derived, Domains...>(std::move(product)) {
    // Since one or more components can be _|_, we need to normalize the
    // representation.
    normalize();
    if (is_bottom()) {
      // No need to reduce the product any further.
      return;
    }
    // Even though no component is _|_, the intersection of the denotations of
    // the components might still be empty. Deciding the emptiness of the
    // intersection usually involves more sophisticated techniques that are
    // specific to the abstract domains in the product. This step is performed
    // by the reduce() method.
    reduce();
  }

  /*
   * We need to define the default constructor in terms of the tuple-taking
   * constructor because the normalized product of the default-constructed
   * component Domains may be _|_.
   */
  ReducedProductAbstractDomain()
      : ReducedProductAbstractDomain(std::tuple<Domains...>{}) {}

  // This method allows the user to explicitly call the reduction operation at
  // any time during the analysis. The `reduce_product()` method implements the
  // mechanics of the reduction operation and should never be called explicitly.
  void reduce() {
    Derived::reduce_product(this->m_product);
    // We don't assume that the reduction operation leaves the representation in
    // a normalized state.
    normalize();
  }

  /*
   * Returns a read-only reference to a component in the tuple.
   */
  template <size_t Index>
  const typename std::tuple_element<Index, std::tuple<Domains...>>::type& get()
      const {
    return std::get<Index>(this->m_product);
  }

  /*
   * Updates a component of the tuple by applying a user-defined operation.
   * Since the reduction may involve costly computations and is not always
   * required depending on the nature of the operation performed, we leave it as
   * an optional step.
   */
  template <
      size_t Index,
      typename Fn = std::function<void(
          typename std::tuple_element<Index, std::tuple<Domains...>>::type*)>>
  void apply(const Fn& operation, bool do_reduction = false) {
    if (is_bottom()) {
      return;
    }
    operation(&std::get<Index>(this->m_product));
    if (this->get<Index>().is_bottom()) {
      this->set_to_bottom();
      return;
    }
    if (do_reduction) {
      reduce();
    }
  }

  bool is_bottom() const {
    // The normalized _|_ element in the product domain has all its components
    // set to _|_, so that we just need to check the first component.
    return this->get<0>().is_bottom();
  }

  // Note that one might want to refine the result of meet and narrow by
  // applying reduce(). The default implementation doesn't call reduce() as it
  // might be too costly to perform this operation after each Meet, or it might
  // even break the termination property of the Narrowing.

  void meet_with(const Derived& other_domain) {
    this->combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.meet_with(other); },
        /* smash_bottom */ true);
  }

  void narrow_with(const Derived& other_domain) {
    this->combine_with(
        other_domain,
        [](auto&& self, auto&& other) { self.narrow_with(other); },
        /* smash_bottom */ true);
  }

 private:
  // Performs the smash-bottom normalization of a tuple of abstract values.
  void normalize() {
    if (this->any_of([](auto&& component) { return component.is_bottom(); })) {
      this->set_to_bottom();
    }
  }
};

namespace rpad_impl {
template <typename Derived, typename... Domains>
class ReducedProductAbstractDomainStaticAssert {
 protected:
  ~ReducedProductAbstractDomainStaticAssert() {
    static_assert(std::is_void_v<decltype(Derived::reduce_product(
                      std::declval<std::tuple<Domains...>&>()))>,
                  "Derived::reduce_product() does not exist");
  }
};
} // namespace rpad_impl

} // namespace sparta
