/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

} // namespace strategy
} // namespace class_merging
