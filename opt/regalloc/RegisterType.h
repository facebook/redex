/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include "FiniteAbstractDomain.h"
#include "IRInstruction.h"

namespace regalloc {

using reg_t = uint16_t;

/*
 * We need to figure out the type of a register in order to generate the
 * right move instruction when spilling / copying them -- e.g. primitives need
 * `move` and refs need `move-object`.
 *
 * The actual Android verifier has a more intricate type lattice (see
 * http://androidxref.com/4.4.2_r2/xref/dalvik/vm/analysis/CodeVerify.h), but
 * this suffices for our needs right now.
 */
enum class RegisterType {
  // Bottom type
  CONFLICT,
  // If const instructions load a zero value, it can be either be a primitive
  // type or a nullptr object ref. We'll only know after looking at other
  // instructions that use that value
  ZERO,
  // Primitive, non-wide type.
  NORMAL,
  WIDE,
  OBJECT,
  // Top type
  UNKNOWN,
  SIZE
};

namespace register_type_impl {

using Lattice = BitVectorLattice<RegisterType,
                                 static_cast<size_t>(RegisterType::SIZE),
                                 boost::hash<RegisterType>>;

extern Lattice lattice;

using Domain =
    FiniteAbstractDomain<RegisterType, Lattice, Lattice::Encoding, &lattice>;

} // namespace register_type_impl

using RegisterTypeDomain = register_type_impl::Domain;

std::string show(RegisterType);

RegisterType dest_reg_type(const IRInstruction*);

RegisterType src_reg_type(const IRInstruction*, reg_t);

/*
 * Generate the right move instruction for a given type.
 */
IRInstruction* gen_move(RegisterType type, reg_t dest, reg_t src);

} // namespace regalloc
