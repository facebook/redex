/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <limits>

#include "ClassHierarchy.h"

#include "IRCode.h"
#include "MergingStrategies.h"
#include "NormalizeConstructor.h"
#include "Trace.h"

namespace class_merging {
namespace strategy {

size_t estimate_vmethods_code_size(const DexClass* cls) {
  size_t estimated_size = 0;
  for (auto method : cls->get_vmethods()) {
    estimated_size += method->get_code()->sum_opcode_sizes();
  }
  return estimated_size;
}

void trace_refs_stats(const GroupStats& group_stats) {
  if (!traceEnabled(CLMG, 5)) {
    return;
  }
  TRACE(CLMG, 5, "============== refs stats ==================");
  for (const auto& stat : group_stats.refs_stats) {
    TRACE(CLMG, 5, "ref %zu cls %zu", stat.first, stat.second);
  }
  TRACE(CLMG, 5, "group ref %zu code size %zu cls %zu", group_stats.ref_count,
        group_stats.estimated_code_size, group_stats.cls_count);
  TRACE(CLMG, 5, "============================================");
}

} // namespace strategy
} // namespace class_merging
