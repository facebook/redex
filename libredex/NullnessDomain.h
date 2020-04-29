/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantAbstractDomain.h"
#include "DexUtil.h"
#include "FiniteAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"

enum Nullness {
  NN_BOTTOM,
  IS_NULL,
  NOT_NULL,
  NN_TOP // Nullable
};

using NullnessLattice = sparta::BitVectorLattice<Nullness, 4, std::hash<int>>;

/*
 *         TOP (Nullable)
 *        /      \
 *      NULL    NOT_NULL
 *        \      /
 *         BOTTOM
 */
extern NullnessLattice lattice;

/*
 * Nullness domain
 *
 * We can use the nullness domain to track the nullness of a given reference
 * type value.
 */
using NullnessDomain = sparta::FiniteAbstractDomain<Nullness,
                                                    NullnessLattice,
                                                    NullnessLattice::Encoding,
                                                    &lattice>;

std::ostream& operator<<(std::ostream& output, const Nullness& nullness);

/*
 * Constant domain
 *
 * Simple domain that tracks the value of integer constants.
 */
using ConstantDomain = sparta::ConstantAbstractDomain<int64_t>;

/*
 * ConstNullness domain
 *
 * A const integer value can have nullness. E.g., const 0 -> NULL.
 */
class ConstNullnessDomain
    : public sparta::ReducedProductAbstractDomain<ConstNullnessDomain,
                                                  ConstantDomain,
                                                  NullnessDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default
  // constructible. We intended to use the default constructors of the base
  // class (via the `using` declaration above), but some compilers fail to
  // catch this. So we insert a redundant '= default'.
  ConstNullnessDomain() = default;

  explicit ConstNullnessDomain(Nullness nullness)
      : ConstNullnessDomain(
            std::make_tuple(ConstantDomain(0), NullnessDomain(nullness))) {}

  explicit ConstNullnessDomain(int64_t v)
      : ConstNullnessDomain(std::make_tuple(
            ConstantDomain(v), NullnessDomain(v == 0 ? IS_NULL : NOT_NULL))) {
    always_assert(v != 0);
  }

  ConstNullnessDomain null() { return ConstNullnessDomain(IS_NULL); }

  static void reduce_product(
      std::tuple<ConstantDomain, NullnessDomain>& /* domains */) {}

  ConstantDomain const_domain() const { return get<0>(); }

  boost::optional<ConstantDomain::ConstantType> get_constant() const {
    return const_domain().get_constant();
  }

  NullnessDomain get_nullness() const { return get<1>(); }
};
