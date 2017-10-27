/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <unordered_set>

#include "DexClass.h"
#include "IRInstruction.h"

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

  DexType* get_throwable_type() const { return m_throwable_type; }

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
  DexType* m_throwable_type{DexType::make_type("Ljava/lang/Throwable;")};
  std::unordered_set<DexType*> m_primitive_type_wrappers{
      {DexType::make_type("Ljava/lang/Boolean;"),
       DexType::make_type("Ljava/lang/Byte;"),
       DexType::make_type("Ljava/lang/Character;"),
       DexType::make_type("Ljava/lang/Double;"),
       DexType::make_type("Ljava/lang/Float;"),
       DexType::make_type("Ljava/lang/Integer;"),
       DexType::make_type("Ljava/lang/Long;"),
       DexType::make_type("Ljava/lang/Short;"),
       DexType::make_type("Ljava/lang/Void;")}};
  DexString* m_wrapper_class_type_field_name{DexString::make_string("TYPE")};
  DexMethodRef* m_java_lang_object_get_class{DexMethod::make_method(
      DexType::make_type("Ljava/lang/Object;"),
      DexString::make_string("getClass"),
      DexProto::make_proto(DexType::make_type("Ljava/lang/Class;"),
                           DexTypeList::make_type_list({})))};
};
