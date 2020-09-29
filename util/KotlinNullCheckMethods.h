/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "DexClass.h"

namespace kotlin_nullcheck_wrapper {
// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull which does not
// require name of the parameter.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckParameter() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter:(Ljava/lang/"
      "Object;)V"));
}

// Wrapper for Kotlin null safety check
// Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull which does not
// require name of the expression.
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression:(Ljava/"
      "lang/Object;)V"));
}

} // namespace kotlin_nullcheck_wrapper
