/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

enum OptReason : int {
  // Inline Passes
  INLINED,

  // RemoveUnusedArgsPass
  CALLSITE_ARGS_REMOVED,
  METHOD_PARAMS_REMOVED,

  // OptimizeEnums
  ENUM_OPTIMIZED,

  // OPT reason count
  N_OPT_REASONS,
};

enum NoptReason : int {
  // Inline Passes
  INL_CROSS_STORE_REFS,
  INL_BLOCK_LISTED_CALLEE,
  INL_BLOCK_LISTED_CALLER,
  INL_EXTERN_CATCH,
  INL_TOO_BIG,
  INL_REQUIRES_API,
  INL_CREATE_VMETH,
  INL_HAS_INVOKE_SUPER,
  INL_UNKNOWN_VIRTUAL,
  INL_UNKNOWN_FIELD,
  INL_TOO_MANY_CALLERS,
  INL_DO_NOT_INLINE,

  // NOPT reason count
  N_NOPT_REASONS,
};
