/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NullnessDomain.h"

NullnessLattice lattice({NN_BOTTOM, UNINITIALIZED, IS_NULL, NOT_NULL, NN_TOP},
                        {{NN_BOTTOM, UNINITIALIZED},
                         {UNINITIALIZED, IS_NULL},
                         {UNINITIALIZED, NOT_NULL},
                         {IS_NULL, NN_TOP},
                         {NOT_NULL, NN_TOP}});

std::ostream& operator<<(std::ostream& output, const Nullness& nullness) {
  switch (nullness) {
  case NN_BOTTOM: {
    output << "BOTTOM";
    break;
  }
  case UNINITIALIZED: {
    output << "UNINIT";
    break;
  }
  case IS_NULL: {
    output << "NULL";
    break;
  }
  case NOT_NULL: {
    output << "NOTNULL";
    break;
  }
  case NN_TOP: {
    output << "NULLABLE";
    break;
  }
  }
  return output;
}
