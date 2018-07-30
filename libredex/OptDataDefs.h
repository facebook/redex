/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

enum OptReason : int {
  // Inline Passes
  OPT_INLINED,

  // RemoveUnusedArgsPass
  OPT_CALLSITE_ARGS_REMOVED,
  OPT_METHOD_PARAMS_REMOVED,
};

/**
 * TODO (anwangster) Categorizing these OPTS/NOPTS is annoying, but splitting
 * up the big OPT enum seems to create bigger issues.
 *
 * So we may use these functions to categorize in the future.
 */
bool is_nopt(OptReason opt);
