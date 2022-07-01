/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "DexClass.h"
#include "MethodUtil.h"

namespace kotlin_nullcheck_wrapper {
constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD_V1_3 =
    "$WrCheckParameter_V1_3";
constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD_V1_4 =
    "$WrCheckParameter_V1_4";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD_V1_3 =
    "$WrCheckExpression_V1_3";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD_V1_4 =
    "$WrCheckExpression_V1_4";

constexpr const char* CHECK_PARAM_NULL_SIGNATURE_V1_3 =
    "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_PARAM_NULL_SIGNATURE_V1_4 =
    "Lkotlin/jvm/internal/Intrinsics;.checkNotNullParameter:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE_V1_3 =
    "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE_V1_4 =
    "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";

constexpr const char* NEW_CHECK_PARAM_NULL_SIGNATURE_V1_3 =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_3:(Ljava/lang/"
    "Object;I)V";
constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE_V1_3 =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3:(Ljava/lang/"
    "Object;)V";

constexpr const char* NEW_CHECK_PARAM_NULL_SIGNATURE_V1_4 =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/"
    "Object;I)V";
constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4 =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_4:(Ljava/lang/"
    "Object;)V";

// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull which does not
// require name of the parameter.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckParameter_V1_3() {
  return static_cast<DexMethod*>(
      DexMethod::get_method(NEW_CHECK_PARAM_NULL_SIGNATURE_V1_3));
}

inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckParameter_V1_4() {
  return static_cast<DexMethod*>(
      DexMethod::get_method(NEW_CHECK_PARAM_NULL_SIGNATURE_V1_4));
}

// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull which does not
// require name of the expression.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_3() {
  return static_cast<DexMethod*>(
      DexMethod::get_method(NEW_CHECK_EXPR_NULL_SIGNATURE_V1_3));
}

inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_4() {
  return static_cast<DexMethod*>(
      DexMethod::get_method(NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4));
}

// This returns methods that are used in Kotlin null assertion.
// These null assertions will take the object that they are checking for
// nullness as first argument and returns void. The value of the object will
// not be null beyond this program point in the execution path.
inline std::unordered_set<DexMethodRef*> get_kotlin_null_assertions() {
  std::unordered_set<DexMethodRef*> null_check_methods;
  DexMethodRef* method;
  method = method::kotlin_jvm_internal_Intrinsics_checkParameterIsNotNull();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = method::kotlin_jvm_internal_Intrinsics_checkNotNullParameter();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = kotlin_nullcheck_wrapper::
      kotlin_jvm_internal_Intrinsics_WrCheckParameter_V1_3();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = kotlin_nullcheck_wrapper::
      kotlin_jvm_internal_Intrinsics_WrCheckParameter_V1_4();
  if (method) {
    null_check_methods.emplace(method);
  }
  method =
      method::kotlin_jvm_internal_Intrinsics_checExpressionValueIsNotNull();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = method::kotlin_jvm_internal_Intrinsics_checkNotNullExpressionValue();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = kotlin_nullcheck_wrapper::
      kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_3();
  if (method) {
    null_check_methods.emplace(method);
  }
  method = kotlin_nullcheck_wrapper::
      kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_4();
  if (method) {
    null_check_methods.emplace(method);
  }
  return null_check_methods;
}

} // namespace kotlin_nullcheck_wrapper
