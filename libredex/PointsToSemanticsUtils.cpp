/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PointsToSemanticsUtils.h"

#include "DexClass.h"
#include "IROpcode.h"
#include "Resolver.h"

bool PointsToSemanticsUtils::is_primitive_type_class_object_retrieval(
    IRInstruction* insn) const {
  always_assert(insn->opcode() == OPCODE_SGET_OBJECT);
  DexField* dex_field = resolve_field(insn->get_field(), FieldSearch::Static);
  return dex_field != nullptr &&
         is_primitive_type_wrapper(dex_field->get_class()) &&
         (dex_field->get_name() == m_wrapper_class_type_field_name);
}

bool PointsToSemanticsUtils::is_get_class_invocation(
    IRInstruction* insn) const {
  return (insn->opcode() == OPCODE_INVOKE_VIRTUAL) &&
         (insn->get_method() == m_java_lang_object_get_class);
}
