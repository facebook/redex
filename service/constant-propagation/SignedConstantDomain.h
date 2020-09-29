/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "SignDomain.h"

using ConstantDomain = sparta::ConstantAbstractDomain<int64_t>;

class SignedConstantDomain
    : public sparta::ReducedProductAbstractDomain<SignedConstantDomain,
                                                  sign_domain::Domain,
                                                  ConstantDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  SignedConstantDomain() = default;

  explicit SignedConstantDomain(int64_t v)
      : SignedConstantDomain(
            std::make_tuple(sign_domain::Domain::top(), ConstantDomain(v))) {}

  explicit SignedConstantDomain(sign_domain::Interval interval)
      : SignedConstantDomain(std::make_tuple(sign_domain::Domain(interval),
                                             ConstantDomain::top())) {}

  static void reduce_product(
      std::tuple<sign_domain::Domain, ConstantDomain>& domains) {
    auto& sdom = std::get<0>(domains);
    auto& cdom = std::get<1>(domains);
    if (sdom.element() == sign_domain::Interval::EQZ) {
      cdom.meet_with(ConstantDomain(0));
      return;
    }
    auto cst = cdom.get_constant();
    if (!cst) {
      return;
    }
    if (!sign_domain::contains(sdom.element(), *cst)) {
      sdom.set_to_bottom();
      return;
    }
    sdom.meet_with(sign_domain::from_int(*cst));
  }

  sign_domain::Domain interval_domain() const { return get<0>(); }

  sign_domain::Interval interval() const { return interval_domain().element(); }

  ConstantDomain constant_domain() const { return get<1>(); }

  boost::optional<ConstantDomain::ConstantType> get_constant() const {
    return constant_domain().get_constant();
  }

  static SignedConstantDomain default_value() {
    return SignedConstantDomain(0);
  }

  /* Return the largest element within the interval. */
  int64_t max_element() const;

  /* Return the smallest element within the interval. */
  int64_t min_element() const;
};
