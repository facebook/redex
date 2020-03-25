/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NullnessDomain.h"

namespace nullness {

NullnessLattice lattice(
    {BOTTOM, IS_NULL, NOT_NULL, TOP},
    {{BOTTOM, IS_NULL}, {BOTTOM, NOT_NULL}, {IS_NULL, TOP}, {NOT_NULL, TOP}});

std::ostream& operator<<(std::ostream& output, const Nullness& nullness) {
  switch (nullness) {
  case BOTTOM: {
    output << "BOTTOM";
    break;
  }
  case IS_NULL: {
    output << "NULL";
    break;
  }
  case NOT_NULL: {
    output << "NOT_NULL";
    break;
  }
  case TOP: {
    output << "NULLABLE";
    break;
  }
  }
  return output;
}

} // namespace nullness
