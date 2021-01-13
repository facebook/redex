/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "DexClass.h"
#include "MethodUtil.h"

namespace kotlin_nullcheck_wrapper {
constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD = "$WrCheckParameter";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD = "$WrCheckExpression";

constexpr const char* CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";

constexpr const char* NEW_CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter:(Ljava/lang/Object;I)V";
constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression:(Ljava/lang/Object;)V";

// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull which does not
// require name of the parameter.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckParameter() {
  return static_cast<DexMethod*>(
      DexMethod::make_method(NEW_CHECK_PARAM_NULL_SIGNATURE));
}

// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull which does not
// require name of the expression.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression() {
  return static_cast<DexMethod*>(
      DexMethod::make_method(NEW_CHECK_EXPR_NULL_SIGNATURE));
}

// This returns methods that are used in Kotlin null assertion.
// These null assertions will take the object that they are checking for
// nullness as first argument and returns void. The value of the object will
// not be null beyond this program point in the execution path.
inline std::unordered_set<DexMethodRef*> get_kotlin_null_assertions() {
  return {method::kotlin_jvm_internal_Intrinsics_checkParameterIsNotNull(),
          method::kotlin_jvm_internal_Intrinsics_checkNotNullParameter(),
          kotlin_nullcheck_wrapper::
              kotlin_jvm_internal_Intrinsics_WrCheckParameter(),
          method::kotlin_jvm_internal_Intrinsics_checExpressionValueIsNotNull(),
          method::kotlin_jvm_internal_Intrinsics_checkNotNullExpressionValue(),
          kotlin_nullcheck_wrapper::
              kotlin_jvm_internal_Intrinsics_WrCheckExpression()};
}

} // namespace kotlin_nullcheck_wrapper
