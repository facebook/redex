/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <limits>

#include "ConstantAbstractDomain.h"
#include "ConstantArrayDomain.h"
#include "ControlFlow.h"
#include "DisjointUnionAbstractDomain.h"
#include "FixpointIterators.h"
#include "HashedAbstractPartition.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "SignDomain.h"

using ConstantDomain = ConstantAbstractDomain<int64_t>;

class SignedConstantDomain
    : public ReducedProductAbstractDomain<SignedConstantDomain,
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

  static SignedConstantDomain top() {
    SignedConstantDomain scd;
    scd.set_to_top();
    return scd;
  }

  static SignedConstantDomain bottom() {
    SignedConstantDomain scd;
    scd.set_to_bottom();
    return scd;
  }

  static SignedConstantDomain default_value() {
    return SignedConstantDomain(0);
  }

  /* Return the largest element within the interval. */
  int64_t max_element() const;

  /* Return the smallest element within the interval. */
  int64_t min_element() const;
};

inline bool operator==(const SignedConstantDomain& x,
                       const SignedConstantDomain& y) {
  return x.equals(y);
}

inline bool operator!=(const SignedConstantDomain& x,
                       const SignedConstantDomain& y) {
  return !(x == y);
}

using ConstantPrimitiveArrayDomain = ConstantArrayDomain<SignedConstantDomain>;

using reg_t = uint32_t;

constexpr reg_t RESULT_REGISTER = std::numeric_limits<reg_t>::max();

// For now, this only represents new-array instructions. Can be extended to
// new-instance in the future.
using AbstractHeapPointer = ConstantAbstractDomain<const IRInstruction*>;

using ConstantArrayHeap =
    PatriciaTreeMapAbstractEnvironment<AbstractHeapPointer::ConstantType,
                                       ConstantPrimitiveArrayDomain>;

using ConstantFieldEnvironment =
    PatriciaTreeMapAbstractEnvironment<DexField*, SignedConstantDomain>;

using ConstantValue =
    DisjointUnionAbstractDomain<SignedConstantDomain, AbstractHeapPointer>;

using ConstantRegisterEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, ConstantValue>;

/*
 * Currently, this models:
 *   - Constant primitive values stored in registers
 *   - Constant array values, referenced by registers that point into the heap
 *   - Constant primitive values stored in fields
 *
 * The array values are stored in an abstract heap. The pointers into the heap
 * are new-array instructions.
 */
class ConstantEnvironment final
    : public ReducedProductAbstractDomain<ConstantEnvironment,
                                          ConstantRegisterEnvironment,
                                          ConstantFieldEnvironment,
                                          ConstantArrayHeap> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  ConstantEnvironment() = default;

  ConstantEnvironment(std::initializer_list<std::pair<reg_t, ConstantValue>> l)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstantRegisterEnvironment(l),
                            ConstantFieldEnvironment(),
                            ConstantArrayHeap())) {}

  static void reduce_product(std::tuple<ConstantRegisterEnvironment,
                                        ConstantFieldEnvironment,
                                        ConstantArrayHeap>&) {}
  /*
   * Getters and setters
   */

  const ConstantRegisterEnvironment& get_register_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const ConstantFieldEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  const ConstantArrayHeap& get_array_heap() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  ConstantValue get(reg_t reg) const {
    return get_register_environment().get(reg);
  }

  SignedConstantDomain get_primitive(reg_t reg) const {
    return get_register_environment().get(reg).get<SignedConstantDomain>();
  }

  AbstractHeapPointer get_array_pointer(reg_t reg) const {
    return get_register_environment().get(reg).get<AbstractHeapPointer>();
  }

  /*
   * Dereference the pointer stored in :reg and return the array it points to.
   */
  ConstantPrimitiveArrayDomain get_array(reg_t reg) const {
    const auto& ptr = get_array_pointer(reg);
    if (ptr.is_top()) {
      return ConstantPrimitiveArrayDomain::top();
    }
    if (ptr.is_bottom()) {
      return ConstantPrimitiveArrayDomain::bottom();
    }
    return get_array_heap().get(*ptr.get_constant());
  }

  SignedConstantDomain get_primitive(DexField* field) const {
    return get_field_environment().get(field);
  }

  ConstantEnvironment& mutate_register_environment(
      std::function<void(ConstantRegisterEnvironment*)> f) {
    apply<0>(f);
    return *this;
  }

  ConstantEnvironment& mutate_field_environment(
      std::function<void(ConstantFieldEnvironment*)> f) {
    apply<1>(f);
    return *this;
  }

  ConstantEnvironment& mutate_array_heap(
      std::function<void(ConstantArrayHeap*)> f) {
    apply<2>(f);
    return *this;
  }

  ConstantEnvironment& set(reg_t reg, const ConstantValue& value) {
    return mutate_register_environment(
        [&](ConstantRegisterEnvironment* env) { env->set(reg, value); });
  }

  /*
   * Store :ptr_val in :reg, and make it point to :value.
   */
  ConstantEnvironment& set_array(
      reg_t reg,
      const AbstractHeapPointer::ConstantType& ptr_val,
      const ConstantPrimitiveArrayDomain& value) {
    set(reg, AbstractHeapPointer(ptr_val));
    mutate_array_heap(
        [&](ConstantArrayHeap* heap) { heap->set(ptr_val, value); });
    return *this;
  }

  /*
   * Bind :value to arr[:idx], where arr is the array referenced by the pointer
   * in register :reg.
   */
  ConstantEnvironment& set_array_binding(reg_t reg,
                                         uint32_t idx,
                                         SignedConstantDomain value) {
    return mutate_array_heap([&](ConstantArrayHeap* heap) {
      auto ptr = get_array_pointer(reg);
      if (!ptr.is_value()) {
        return;
      }
      heap->update(*ptr.get_constant(),
                   [&](const ConstantPrimitiveArrayDomain& arr) {
                     auto copy = arr;
                     copy.set(idx, value);
                     return copy;
                   });
    });
  }

  ConstantEnvironment& set(DexField* field, const SignedConstantDomain& value) {
    return mutate_field_environment(
        [&](ConstantFieldEnvironment* env) { env->set(field, value); });
  }

  ConstantEnvironment& clear_field_environment() {
    return mutate_field_environment(
        [](ConstantFieldEnvironment* env) { env->set_to_top(); });
  }

  static ConstantEnvironment top() { return ConstantEnvironment(); }

  static ConstantEnvironment bottom() {
    ConstantEnvironment env;
    env.set_to_bottom();
    return env;
  }
};

using ConstantStaticFieldPartition =
    HashedAbstractPartition<const DexField*, SignedConstantDomain>;

using ConstantMethodPartition =
    HashedAbstractPartition<const DexMethod*, SignedConstantDomain>;
