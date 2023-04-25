/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantAbstractDomain.h"
#include "DexUtil.h"
#include "DisjointUnionAbstractDomain.h"
#include "FiniteAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"

enum Nullness {
  NN_BOTTOM,
  UNINITIALIZED, // The elements of a newly allocated array is not NULL or
                 // NOT_NULL
  IS_NULL,
  NOT_NULL,
  NN_TOP // Nullable
};

using NullnessLattice = sparta::BitVectorLattice<Nullness,
                                                 /* kCardinality */ 5>;

/*
 *         TOP (Nullable)
 *        /      \
 *      NULL    NOT_NULL
 *        \      /
 *      UNINITIALIZED
 *           |
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
      : ConstNullnessDomain(std::make_tuple(
            nullness == IS_NULL ? ConstantDomain(0) : ConstantDomain::top(),
            NullnessDomain(nullness))) {}

  explicit ConstNullnessDomain(int64_t v)
      : ConstNullnessDomain(std::make_tuple(
            ConstantDomain(v), NullnessDomain(v == 0 ? IS_NULL : NOT_NULL))) {}

  ConstNullnessDomain null() { return ConstNullnessDomain(IS_NULL); }

  static void reduce_product(
      std::tuple<ConstantDomain, NullnessDomain>& /* domains */) {}

  ConstantDomain const_domain() const { return get<0>(); }

  boost::optional<ConstantDomain::ConstantType> get_constant() const {
    return const_domain().get_constant();
  }

  NullnessDomain get_nullness() const { return get<1>(); }
};

/*
 * Spec wise the max size of a Java array is
 * std::numeric_limits<int32_t>::max() - 8. Reference:
 * http://hg.openjdk.java.net/jdk7/jdk7/jdk/rev/ec45423a4700#l5.12
 *
 * However, for performance reasons, we don't want to allocate a Domain this
 * large. We cap the size of the array elements at 1000.
 */
constexpr int64_t JAVA_ARRAY_SIZE_MAX = 1000;

class ArrayNullnessDomain final
    : public sparta::ReducedProductAbstractDomain<
          ArrayNullnessDomain,
          NullnessDomain /* nullness of the array object */,
          sparta::ConstantAbstractDomain<uint32_t> /* array length */,
          sparta::PatriciaTreeMapAbstractEnvironment<
              uint32_t,
              NullnessDomain> /* array elements */> {

 public:
  using ArrayLengthDomain = sparta::ConstantAbstractDomain<uint32_t>;
  using ElementsNullness =
      sparta::PatriciaTreeMapAbstractEnvironment<uint32_t, NullnessDomain>;
  using BaseType = sparta::ReducedProductAbstractDomain<ArrayNullnessDomain,
                                                        NullnessDomain,
                                                        ArrayLengthDomain,
                                                        ElementsNullness>;
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  ArrayNullnessDomain() = default;

  static void reduce_product(
      std::tuple<NullnessDomain, ArrayLengthDomain, ElementsNullness>&
          product) {
    if (std::get<1>(product).is_top()) {
      std::get<2>(product).set_to_top();
    }
  }

  explicit ArrayNullnessDomain(uint32_t length)
      : ReducedProductAbstractDomain(std::make_tuple(NullnessDomain(NOT_NULL),
                                                     ArrayLengthDomain(length),
                                                     ElementsNullness())) {
    mutate_elements([length](ElementsNullness* elements) {
      for (size_t i = 0; i < length; ++i) {
        elements->set(i, NullnessDomain(UNINITIALIZED));
      }
    });
    reduce();
  }

  NullnessDomain get_nullness() const { return get<0>(); }

  boost::optional<uint32_t> get_length() const {
    return get<1>().get_constant();
  }

  ElementsNullness get_elements() const { return get<2>(); }

  void reset_elements() {
    apply<2>([](auto elements) { elements->set_to_top(); });
  }

  NullnessDomain get_element(uint32_t idx) const {
    return get_elements().get(idx);
  }

  ArrayNullnessDomain& set_element(uint32_t idx, const NullnessDomain& domain) {
    if (is_top() || is_bottom()) {
      return *this;
    }
    auto length = get_length();
    if (!length || idx >= *length) {
      return *this;
    }
    return this->mutate_elements([idx, domain](ElementsNullness* elements) {
      elements->set(idx, domain);
    });
  }

  void join_with(const ArrayNullnessDomain& other) {
    BaseType::join_with(other);
    reduce();
  }

  void widen_with(const ArrayNullnessDomain& other) {
    BaseType::widen_with(other);
    reduce();
  }

  static bool is_valid_array_size(boost::optional<int64_t>& val) {
    return val && *val >= 0 && *val <= JAVA_ARRAY_SIZE_MAX;
  }

  static bool is_valid_array_idx(boost::optional<int64_t>& val) {
    return val && *val >= 0 && *val < JAVA_ARRAY_SIZE_MAX;
  }

 private:
  ArrayNullnessDomain& mutate_elements(
      std::function<void(ElementsNullness*)> f) {
    this->template apply<2>(std::move(f));
    return *this;
  }
};

using ArrayConstNullnessDomain =
    sparta::DisjointUnionAbstractDomain<ConstNullnessDomain,
                                        ArrayNullnessDomain>;
