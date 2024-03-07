/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/ReducedProductAbstractDomain.h>

#include "DexClass.h"
#include "Show.h"
#include "SignedConstantDomain.h"

using NewObjectInstructionDomain =
    sparta::ConstantAbstractDomain<const IRInstruction*>;

using NewObjectTypeDomain = sparta::ConstantAbstractDomain<const DexType*>;

// NewObjectDomain simply represents an object (either a class instance or an
// array) created at a particular instruction. In the case of an array, it also
// captures the array length domain value. Any other mutable properties of an
// object (fields, array elements) are not represented.
class NewObjectDomain final
    : public sparta::ReducedProductAbstractDomain<NewObjectDomain,
                                                  NewObjectInstructionDomain,
                                                  NewObjectTypeDomain,
                                                  SignedConstantDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;
  NewObjectDomain() = default;

  explicit NewObjectDomain(const IRInstruction* insn)
      : ReducedProductAbstractDomain(
            std::make_tuple(NewObjectInstructionDomain(insn),
                            NewObjectTypeDomain(insn->get_type()),
                            SignedConstantDomain::top())) {}

  explicit NewObjectDomain(const IRInstruction* insn,
                           SignedConstantDomain array_length)
      : ReducedProductAbstractDomain(
            std::make_tuple(NewObjectInstructionDomain(insn),
                            NewObjectTypeDomain(insn->get_type()),
                            std::move(array_length))) {}

  static void reduce_product(std::tuple<NewObjectInstructionDomain,
                                        NewObjectTypeDomain,
                                        SignedConstantDomain>&) {}

  const IRInstruction* get_new_object_insn() const {
    auto c = ReducedProductAbstractDomain::get<0>().get_constant();
    return c ? *c : nullptr;
  }

  const DexType* get_type() const {
    auto c = ReducedProductAbstractDomain::get<1>().get_constant();
    return c ? *c : nullptr;
  }

  SignedConstantDomain get_array_length() const {
    return ReducedProductAbstractDomain::get<2>();
  }
};
