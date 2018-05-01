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
#include "SignedConstantDomain.h"

/*
 * The definitions in this file serve to abstractly model:
 *   - Constant primitive values stored in registers
 *   - Constant array values, referenced by registers that point into the heap
 *   - Constant primitive values stored in fields
 */

using reg_t = uint32_t;

constexpr reg_t RESULT_REGISTER = std::numeric_limits<reg_t>::max();

/*****************************************************************************
 * Abstract stack / environment values.
 *****************************************************************************/

/*
 * This represents an object that is uniquely referenced by a single static
 * field. This enables us to compare these objects easily -- we can determine
 * whether two different SingletonObjectDomain elements are equal just based
 * on their representation in the abstract environment, without needing to
 * check if they are pointing to the same object in the abstract heap.
 */
using SingletonObjectDomain = ConstantAbstractDomain<const DexField*>;

// For now, this only represents new-array instructions. Can be extended to
// new-instance in the future.
using AbstractHeapPointer = ConstantAbstractDomain<const IRInstruction*>;

using ConstantValue = DisjointUnionAbstractDomain<SignedConstantDomain,
                                                  SingletonObjectDomain,
                                                  AbstractHeapPointer>;

using ConstantFieldEnvironment =
    PatriciaTreeMapAbstractEnvironment<const DexField*, ConstantValue>;

using ConstantRegisterEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, ConstantValue>;

/*****************************************************************************
 * Heap values.
 *****************************************************************************/

using ConstantPrimitiveArrayDomain = ConstantArrayDomain<SignedConstantDomain>;

using ConstantArrayHeap =
    PatriciaTreeMapAbstractEnvironment<AbstractHeapPointer::ConstantType,
                                       ConstantPrimitiveArrayDomain>;

/*****************************************************************************
 * Combined model of the abstract stack and heap.
 *****************************************************************************/

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

  AbstractHeapPointer get_pointer(reg_t reg) const {
    return get_register_environment().get(reg).get<AbstractHeapPointer>();
  }

  SingletonObjectDomain get_singleton(reg_t reg) const {
    return get_register_environment().get(reg).get<SingletonObjectDomain>();
  }

  /*
   * Dereference the pointer stored in :reg and return the array it points to.
   */
  ConstantPrimitiveArrayDomain get_array(reg_t reg) const {
    const auto& ptr = get_pointer(reg);
    if (ptr.is_top()) {
      return ConstantPrimitiveArrayDomain::top();
    }
    if (ptr.is_bottom()) {
      return ConstantPrimitiveArrayDomain::bottom();
    }
    return get_array_heap().get(*ptr.get_constant());
  }

  ConstantValue get(DexField* field) const {
    return get_field_environment().get(field);
  }

  SignedConstantDomain get_primitive(DexField* field) const {
    return get(field).get<SignedConstantDomain>();
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
      auto ptr = get_pointer(reg);
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

  ConstantEnvironment& set(DexField* field, const ConstantValue& value) {
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
