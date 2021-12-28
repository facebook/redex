/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantAbstractDomain.h"
#include "IntervalDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "SignDomain.h"

using ConstantDomain = sparta::ConstantAbstractDomain<int64_t>;
using NumericIntervalType = int32_t;
using NumericIntervalDomain = sparta::IntervalDomain<NumericIntervalType>;

// input interval must not be empty
inline NumericIntervalDomain numeric_interval_domain_from_int(int64_t min,
                                                              int64_t max) {
  always_assert(min <= max);
  if (min <= NumericIntervalDomain::MIN) {
    if (max >= NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::top();
    } else if (max > NumericIntervalDomain::MIN) {
      return NumericIntervalDomain::bounded_above(max);
    } else {
      return NumericIntervalDomain::low();
    }
  } else {
    if (max < NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::finite(min, max);
    } else if (min < NumericIntervalDomain::MAX) {
      return NumericIntervalDomain::bounded_below(min);
    } else {
      return NumericIntervalDomain::high();
    }
  }
}

// input interval must not be empty
inline NumericIntervalDomain numeric_interval_domain_from_interval(
    sign_domain::Interval interval) {
  switch (interval) {
  case sign_domain::Interval::EMPTY:
    not_reached();
  case sign_domain::Interval::EQZ:
    return NumericIntervalDomain::finite(0, 0);
  case sign_domain::Interval::LEZ:
    return NumericIntervalDomain::bounded_above(0);
  case sign_domain::Interval::LTZ:
    return NumericIntervalDomain::bounded_above(-1);
  case sign_domain::Interval::GEZ:
    return NumericIntervalDomain::bounded_below(0);
  case sign_domain::Interval::GTZ:
    return NumericIntervalDomain::bounded_below(1);
  case sign_domain::Interval::ALL:
  case sign_domain::Interval::NEZ:
    return NumericIntervalDomain::top();
  case sign_domain::Interval::SIZE:
    not_reached();
  }
}

// input domain must not be bottom
inline sign_domain::Domain sign_from_numeric_interval_domain(
    const NumericIntervalDomain& domain) {
  auto lower_bound = domain.lower_bound();
  if (lower_bound > 0) {
    return sign_domain::Domain(sign_domain::Interval::GTZ);
  }
  auto upper_bound = domain.upper_bound();
  if (upper_bound < 0) {
    return sign_domain::Domain(sign_domain::Interval::LTZ);
  }
  if (lower_bound == upper_bound && lower_bound == 0) {
    return sign_domain::Domain(sign_domain::Interval::EQZ);
  }
  if (lower_bound >= 0) {
    return sign_domain::Domain(sign_domain::Interval::GEZ);
  }
  if (upper_bound <= 0) {
    return sign_domain::Domain(sign_domain::Interval::LEZ);
  }
  return sign_domain::Domain::top();
}

// input domain must not be bottom
inline ConstantDomain constant_from_numeric_interval_domain(
    const NumericIntervalDomain& domain) {
  auto lower_bound = domain.lower_bound();
  auto upper_bound = domain.upper_bound();
  if (lower_bound == upper_bound && lower_bound > NumericIntervalDomain::MIN &&
      upper_bound < NumericIntervalDomain::MAX) {
    return ConstantDomain(lower_bound);
  }
  return ConstantDomain::top();
}

class SignedConstantDomain
    : public sparta::ReducedProductAbstractDomain<SignedConstantDomain,
                                                  sign_domain::Domain,
                                                  NumericIntervalDomain,
                                                  ConstantDomain> {
 public:
  using BaseType = sparta::ReducedProductAbstractDomain<SignedConstantDomain,
                                                        sign_domain::Domain,
                                                        NumericIntervalDomain,
                                                        ConstantDomain>;
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  SignedConstantDomain() = default;

  explicit SignedConstantDomain(int64_t v)
      : SignedConstantDomain(
            std::make_tuple(sign_domain::Domain::top(),
                            numeric_interval_domain_from_int(v, v),
                            ConstantDomain(v))) {}

  explicit SignedConstantDomain(sign_domain::Interval interval)
      : SignedConstantDomain(
            std::make_tuple(sign_domain::Domain(interval),
                            numeric_interval_domain_from_interval(interval),
                            ConstantDomain::top())) {}

  explicit SignedConstantDomain(int64_t min, int64_t max)
      : SignedConstantDomain(
            std::make_tuple(sign_domain::Domain::top(),
                            numeric_interval_domain_from_int(min, max),
                            ConstantDomain::top())) {}

  static void reduce_product(
      std::tuple<sign_domain::Domain, NumericIntervalDomain, ConstantDomain>&
          domains) {
    auto& sdom = std::get<0>(domains);
    auto& idom = std::get<1>(domains);
    auto& cdom = std::get<2>(domains);

    // propagate sdom
    if (!sdom.is_bottom()) {
      idom.meet_with(numeric_interval_domain_from_interval(sdom.element()));
      if (sdom.element() == sign_domain::Interval::EQZ) {
        cdom.meet_with(ConstantDomain(0));
        return;
      }
    }

    // propagate idom
    if (!idom.is_bottom()) {
      sdom.meet_with(sign_from_numeric_interval_domain(idom));
      cdom.meet_with(constant_from_numeric_interval_domain(idom));
    }

    // propagate cdom
    auto cst = cdom.get_constant();
    if (cst) {
      sdom.meet_with(sign_domain::from_int(*cst));
      idom.meet_with(numeric_interval_domain_from_int(*cst, *cst));
    }
  }

  void meet_with(const SignedConstantDomain& other) override {
    BaseType::meet_with(other);
    reduce();
  }

  void narrow_with(const SignedConstantDomain& /* other */) override {
    throw std::runtime_error("narrow_with not implemented!");
  }

  sign_domain::Domain interval_domain() const { return get<0>(); }

  sign_domain::Interval interval() const { return interval_domain().element(); }

  ConstantDomain constant_domain() const { return get<2>(); }

  NumericIntervalDomain numeric_interval_domain() const { return get<1>(); }

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
