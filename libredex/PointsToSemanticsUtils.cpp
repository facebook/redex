/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PointsToSemanticsUtils.h"

#include "DexClass.h"
#include "DexOpcode.h"

bool PointsToSemanticsUtils::is_primitive_type_class_object_retrieval(
    IRInstruction* insn) const {
  always_assert(insn->opcode() == OPCODE_SGET_OBJECT);
  DexField* dex_field = insn->get_field();
  DexType* dex_type = dex_field->get_class();
  return is_primitive_type_wrapper(dex_type) &&
         (dex_field->get_name() == m_wrapper_class_type_field_name);
}
