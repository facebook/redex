/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

#include <unordered_map>

namespace field_op_tracker {

struct FieldStats {
  // Number of instructions which read a field in the entire program.
  size_t reads{0};
  // Number of instructions which read this field outside of a <clinit> or
  // <init>.
  size_t reads_outside_init{0};
  // Number of instructions which write a field in the entire program.
  size_t writes{0};
};

using FieldStatsMap = std::unordered_map<DexField*, FieldStats>;

FieldStatsMap analyze(const Scope& scope);

using NonZeroWrittenFields = std::unordered_set<DexField*>;

NonZeroWrittenFields analyze_non_zero_writes(const Scope& scope);
} // namespace field_op_tracker
