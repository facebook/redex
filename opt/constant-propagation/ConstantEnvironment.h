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

/*
 * We have a number of environments with "Constant" in their names. The naming
 * scheme is as follows: when the word comes before Constant, it is referring
 * to the variable (key); when it comes after it is referring to the domain
 * (value).
 */

template <typename Variable>
using ConstantPrimitiveEnvironment =
    PatriciaTreeMapAbstractEnvironment<Variable, SignedConstantDomain>;

// For now, this only represents new-array instructions. Can be extended to
// new-instance in the future.
using AbstractHeapPointer = ConstantAbstractDomain<const IRInstruction*>;

template <typename Variable>
using ConstantArrayEnvironment =
    PatriciaTreeMapAbstractEnvironment<Variable, AbstractHeapPointer>;

using ConstantArrayHeap =
    PatriciaTreeMapAbstractEnvironment<AbstractHeapPointer::ConstantType,
                                       ConstantPrimitiveArrayDomain>;

using FieldConstantEnvironment =
    PatriciaTreeMapAbstractEnvironment<DexField*, SignedConstantDomain>;

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
                                          ConstantPrimitiveEnvironment<reg_t>,
                                          ConstantArrayEnvironment<reg_t>,
                                          FieldConstantEnvironment,
                                          ConstantArrayHeap> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  ConstantEnvironment() = default;

  ConstantEnvironment(
      std::initializer_list<std::pair<reg_t, SignedConstantDomain>> l)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstantPrimitiveEnvironment<reg_t>(l),
                            ConstantArrayEnvironment<reg_t>(),
                            FieldConstantEnvironment(),
                            ConstantArrayHeap())) {}

  static void reduce_product(std::tuple<ConstantPrimitiveEnvironment<reg_t>,
                                        ConstantArrayEnvironment<reg_t>,
                                        FieldConstantEnvironment,
                                        ConstantArrayHeap>&) {}
  /*
   * Getters and setters
   */

  const ConstantPrimitiveEnvironment<reg_t>& get_primitive_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const ConstantArrayEnvironment<reg_t>& get_array_environment() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  const FieldConstantEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  const ConstantArrayHeap& get_array_heap() const {
    return ReducedProductAbstractDomain::get<3>();
  }

  SignedConstantDomain get_primitive(reg_t reg) const {
    return get_primitive_environment().get(reg);
  }

  AbstractHeapPointer get_array_pointer(reg_t reg) const {
    return get_array_environment().get(reg);
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

  ConstantEnvironment& mutate_primitive_environment(
      std::function<void(ConstantPrimitiveEnvironment<reg_t>*)> f) {
    apply<0>(f);
    return *this;
  }

  ConstantEnvironment& mutate_array_environment(
      std::function<void(ConstantArrayEnvironment<reg_t>*)> f) {
    apply<1>(f);
    return *this;
  }

  ConstantEnvironment& mutate_field_environment(
      std::function<void(FieldConstantEnvironment*)> f) {
    apply<2>(f);
    return *this;
  }

  ConstantEnvironment& mutate_array_heap(
      std::function<void(ConstantArrayHeap*)> f) {
    apply<3>(f);
    return *this;
  }

  ConstantEnvironment& set_primitive(reg_t reg,
                                     const SignedConstantDomain& value) {
    // If the register was bound to an array pointer before, we must unbind it
    mutate_array_environment([&](ConstantArrayEnvironment<reg_t>* env) {
      env->set(reg, AbstractHeapPointer::top());
    });
    return mutate_primitive_environment(
        [&](ConstantPrimitiveEnvironment<reg_t>* env) {
          env->set(reg, value);
        });
  }

  ConstantEnvironment& set_array_pointer(reg_t reg,
                                         const AbstractHeapPointer& ptr) {
    // If the register was bound to a primitive value before, we must unbind it
    mutate_primitive_environment([&](ConstantPrimitiveEnvironment<reg_t>* env) {
      env->set(reg, SignedConstantDomain::top());
    });
    return mutate_array_environment(
        [&](ConstantArrayEnvironment<reg_t>* env) { env->set(reg, ptr); });
  }

  /*
   * Store :ptr_val in :reg, and make it point to :value.
   */
  ConstantEnvironment& set_array(
      reg_t reg,
      const AbstractHeapPointer::ConstantType& ptr_val,
      const ConstantPrimitiveArrayDomain& value) {
    set_array_pointer(reg, AbstractHeapPointer(ptr_val));
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

  /*
   * Regardless of the type of the register, bind it to Top.
   */
  ConstantEnvironment& set_register_to_top(reg_t reg) {
    set_primitive(reg, SignedConstantDomain::top());
    set_array_pointer(reg, AbstractHeapPointer::top());
    return *this;
  }

  ConstantEnvironment& set_primitive(DexField* field,
                                     const SignedConstantDomain& value) {
    return mutate_field_environment(
        [&](FieldConstantEnvironment* env) { env->set(field, value); });
  }

  ConstantEnvironment& clear_field_environment() {
    return mutate_field_environment(
        [](FieldConstantEnvironment* env) { env->set_to_top(); });
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
