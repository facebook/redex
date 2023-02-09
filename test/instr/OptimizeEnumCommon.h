/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>

#include "DexClass.h"

// The Java input only uses switches, but in general they may transformed
// into any equivalent branch. (We then apply a further transformation
// onto these branches to optimize enums).
//
// For verifying our transforms, we will gather up all branch comparisons
// to consts rather than strictly switches (or what we deem switch-like
// branching).
//
// Also tracked is if the const is being compared against the result of a
// virtual call or array lookup. This allows checking that the comparison
// is being done against an ordinal or switchmap.

enum class BranchSource {
  ArrayGet,
  ArrayGetOrConstMinus1,
  VirtualCall,
  VirtualCallOrConstMinus1,
};

using BranchCase = std::tuple<BranchSource, int64_t>;

std::set<BranchCase> collect_const_branch_cases(DexMethodRef* method_ref);
