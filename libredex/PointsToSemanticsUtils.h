/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "DexClass.h"
#include "IRInstruction.h"
#include "TypeUtil.h"

/*
 * This class contains a set of utility functions used to build the points-to
 * semantics, mostly for stubbing external APIs. It also serves as a cache for
 * common types and methods from the standard API (like collections). Since
 * these entities are produced by the global context `g_redex`, it is better to
 * precompute them for faster retrieval. Note that we couldn't achieve this
 * using just static functions and variables, as `g_redex` is initialized at
 * runtime.
 */
class PointsToSemanticsUtils final {
 public:
  PointsToSemanticsUtils() = default;

  PointsToSemanticsUtils(const PointsToSemanticsUtils& other) = delete;

  PointsToSemanticsUtils& operator=(const PointsToSemanticsUtils& other) =
      delete;

  bool is_primitive_type_wrapper(DexType* dex_type) const {
    return m_primitive_type_wrappers.count(dex_type) > 0;
  }

  // Checks whether an sget-object instruction accesses the `TYPE` field of a
  // primitive type's wrapper class.
  bool is_primitive_type_class_object_retrieval(IRInstruction* insn) const;

  // Checks whether the method java.lang.Object#getClass() is called by a method
  // invocation operation.
  bool is_get_class_invocation(IRInstruction* insn) const;

 private:
  std::unordered_set<DexType*> m_primitive_type_wrappers{
      type::java_lang_Boolean(),   type::java_lang_Byte(),
      type::java_lang_Character(), type::java_lang_Double(),
      type::java_lang_Float(),     type::java_lang_Integer(),
      type::java_lang_Long(),      type::java_lang_Short(),
      type::java_lang_Void()};
  const DexString* m_wrapper_class_type_field_name{
      DexString::make_string("TYPE")};
  DexMethodRef* m_java_lang_object_get_class{DexMethod::make_method(
      DexType::make_type("Ljava/lang/Object;"),
      DexString::make_string("getClass"),
      DexProto::make_proto(DexType::make_type("Ljava/lang/Class;"),
                           DexTypeList::make_type_list({})))};
};
