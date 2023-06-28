/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <limits>
#include <utility>

#include "ConstantAbstractDomain.h"
#include "ConstantArrayDomain.h"
#include "ControlFlow.h"
#include "DisjointUnionAbstractDomain.h"
#include "DisjointUnionWithSignedConstantDomain.h"
#include "HashedAbstractPartition.h"
#include "HashedSetAbstractDomain.h"
#include "ObjectDomain.h"
#include "ObjectWithImmutAttr.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "SignedConstantDomain.h"
#include "SingletonObject.h"

/*
 * The definitions in this file serve to abstractly model:
 *   - Constant primitive values stored in registers
 *   - Constant array values, referenced by registers that point into the heap
 *   - Constant primitive values stored in fields
 */

/*****************************************************************************
 * Abstract stack / environment values.
 *****************************************************************************/

using IntegerSetDomain = sparta::HashedSetAbstractDomain<int64_t>;

using StringSetDomain = sparta::PatriciaTreeSetAbstractDomain<const DexString*>;

using StringDomain = sparta::ConstantAbstractDomain<const DexString*>;

using ConstantClassObjectDomain =
    sparta::ConstantAbstractDomain<const DexType*>;

using ConstantInjectionIdDomain = sparta::ConstantAbstractDomain<int32_t>;

/*
 * This represents a new-instance or new-array instruction.
 */
using AbstractHeapPointer =
    sparta::ConstantAbstractDomain<const IRInstruction*>;

/*
 * Identifies domains whose members are compatible with NEZ.
 */
class is_object_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SingletonObjectDomain&) const { return true; }

  bool operator()(const StringDomain&) const { return true; }

  bool operator()(const ConstantClassObjectDomain&) const { return true; }

  bool operator()(const ObjectWithImmutAttrDomain&) const { return true; }

  bool operator()(const AbstractHeapPointer&) const { return true; }

  template <typename Domain>
  bool operator()(const Domain&) const {
    return false;
  }
};

// TODO: Refactor so that we don't have to list every single possible
// sub-Domain here.
using ConstantValue =
    DisjointUnionWithSignedConstantDomain<is_object_visitor,
                                          SingletonObjectDomain,
                                          IntegerSetDomain,
                                          StringSetDomain,
                                          StringDomain,
                                          ConstantClassObjectDomain,
                                          ConstantInjectionIdDomain,
                                          ObjectWithImmutAttrDomain,
                                          AbstractHeapPointer>;

struct ConstantValueDefaultValue {
  ConstantValue operator()() { return SignedConstantDomain(0); }
};

// For storing non-escaping static and instance fields.
using FieldEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<const DexField*, ConstantValue>;

using ConstantRegisterEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, ConstantValue>;

/*****************************************************************************
 * Heap values.
 * ConstantPropagationPass and IPCP do not support heap stores properly. Use
 * LocalPointersAnalysis for local mutable objects analysis.
 *****************************************************************************/

using ConstantValueArrayDomain =
    ConstantArrayDomain<ConstantValue, ConstantValueDefaultValue>;

using ConstantObjectDomain = ObjectDomain<ConstantValue>;

using HeapValue = sparta::DisjointUnionAbstractDomain<ConstantValueArrayDomain,
                                                      ConstantObjectDomain>;

using ConstantHeap = sparta::PatriciaTreeMapAbstractEnvironment<
    AbstractHeapPointer::ConstantType,
    HeapValue>;

/*****************************************************************************
 * Combined model of the abstract stack and heap.
 *****************************************************************************/

class ConstantEnvironment final
    : public sparta::ReducedProductAbstractDomain<ConstantEnvironment,
                                                  ConstantRegisterEnvironment,
                                                  FieldEnvironment,
                                                  ConstantHeap> {
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
                            FieldEnvironment(),
                            ConstantHeap())) {}

  static void reduce_product(std::tuple<ConstantRegisterEnvironment,
                                        FieldEnvironment,
                                        ConstantHeap>&) {}
  /*
   * Getters and setters
   */

  const ConstantRegisterEnvironment& get_register_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const FieldEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  const ConstantHeap& get_heap() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  ConstantValue get(reg_t reg) const {
    return get_register_environment().get(reg);
  }

  template <typename Domain>
  Domain get(reg_t reg) const {
    return get_register_environment().get(reg).template get<Domain>();
  }

  ConstantValue get(DexField* field) const {
    return get_field_environment().get(field);
  }

  template <typename Domain>
  Domain get(const DexField* field) const {
    return get_field_environment().get(field).template get<Domain>();
  }

  /*
   * Dereference :ptr and return the HeapValue that it points to.
   */
  template <typename HeapValue>
  HeapValue get_pointee(const AbstractHeapPointer& ptr) const {
    if (ptr.is_top()) {
      return HeapValue::top();
    }
    if (ptr.is_bottom()) {
      return HeapValue::bottom();
    }
    return get_heap().get(*ptr.get_constant()).get<HeapValue>();
  }

  /*
   * Dereference the pointer stored in :reg and return the HeapValue that it
   * points to.
   */
  template <typename HeapValue>
  HeapValue get_pointee(reg_t reg) const {
    const auto& ptr = get<AbstractHeapPointer>(reg);
    return get_pointee<HeapValue>(ptr);
  }

  ConstantEnvironment& mutate_register_environment(
      std::function<void(ConstantRegisterEnvironment*)> f) {
    apply<0>(std::move(f));
    return *this;
  }

  ConstantEnvironment& mutate_field_environment(
      std::function<void(FieldEnvironment*)> f) {
    apply<1>(std::move(f));
    return *this;
  }

  ConstantEnvironment& mutate_heap(std::function<void(ConstantHeap*)> f) {
    apply<2>(std::move(f));
    return *this;
  }

  ConstantEnvironment& set(reg_t reg, const ConstantValue& value) {
    return mutate_register_environment(
        [&](ConstantRegisterEnvironment* env) { env->set(reg, value); });
  }

  ConstantEnvironment& set(const DexField* field, const ConstantValue& value) {
    return mutate_field_environment(
        [&](FieldEnvironment* env) { env->set(field, value); });
  }

  /*
   * Store :ptr_val in :reg, and make it point to :value.
   */
  ConstantEnvironment& new_heap_value(
      reg_t reg,
      const AbstractHeapPointer::ConstantType& ptr_val,
      const HeapValue& value) {
    set(reg, AbstractHeapPointer(ptr_val));
    mutate_heap([&](ConstantHeap* heap) { heap->set(ptr_val, value); });
    return *this;
  }

  /*
   * Bind :value to arr[:idx], where arr is the array referenced by the pointer
   * in register :reg.
   */
  ConstantEnvironment& set_array_binding(reg_t reg,
                                         uint32_t idx,
                                         const ConstantValue& value) {
    return mutate_heap([&](ConstantHeap* heap) {
      auto ptr = get<AbstractHeapPointer>(reg);
      if (!ptr.is_value()) {
        return;
      }
      heap->update(*ptr.get_constant(), [&](const HeapValue& arr) {
        auto copy = arr.get<ConstantValueArrayDomain>();
        copy.set(idx, value);
        return copy;
      });
    });
  }

  ConstantEnvironment& set_object_field(reg_t reg,
                                        const DexField* field,
                                        const ConstantValue& value) {
    return mutate_heap([&](ConstantHeap* heap) {
      auto ptr = get<AbstractHeapPointer>(reg);
      if (!ptr.is_value()) {
        return;
      }
      heap->update(*ptr.get_constant(), [&](const HeapValue& arr) {
        auto copy = arr.get<ConstantObjectDomain>();
        copy.set(field, value);
        return copy;
      });
    });
  }

  ConstantEnvironment& clear_field_environment() {
    return mutate_field_environment(
        [](FieldEnvironment* env) { env->set_to_top(); });
  }
};

/*
 * For modeling the stack + heap at method return statements.
 */
class ReturnState final
    : public sparta::ReducedProductAbstractDomain<ReturnState,
                                                  ConstantValue,
                                                  ConstantHeap> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  ReturnState() = default;

  ReturnState(const ConstantValue& value, const ConstantHeap& heap)
      : ReducedProductAbstractDomain(std::make_tuple(value, heap)) {}

  static void reduce_product(std::tuple<ConstantValue, ConstantHeap>&) {}

  ConstantValue get_value() { return ReducedProductAbstractDomain::get<0>(); }

  template <typename Domain>
  Domain get_value() {
    return ReducedProductAbstractDomain::get<0>().template get<Domain>();
  }

  ConstantHeap get_heap() { return ReducedProductAbstractDomain::get<1>(); }
};
