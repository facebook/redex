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
enum NullErrSrc {
  UNKNOWN_SRC = 0,
  LOAD_PARAM,
  CONST,
  INSTANCE_FIELD,
  STATIC_FIELD,
  ARRAY_ELEMENT,
  INVOKE_RETURN,
  CHECK_CAST
};

constexpr std::initializer_list<NullErrSrc> all_NullErrSrc = {
    UNKNOWN_SRC,  LOAD_PARAM,    CONST,         INSTANCE_FIELD,
    STATIC_FIELD, ARRAY_ELEMENT, INVOKE_RETURN, CHECK_CAST};

inline std::string get_err_msg(NullErrSrc err) {
  switch (err) {
  case NullErrSrc::UNKNOWN_SRC:
    return "UNKNOWN";
  case NullErrSrc::LOAD_PARAM:
    return "LOAD_PARAM";
  case NullErrSrc::CONST:
    return "CONST";
  case NullErrSrc::INSTANCE_FIELD:
    return "INSTANCE_FIELD";
  case NullErrSrc::STATIC_FIELD:
    return "STATIC_FIELD";
  case NullErrSrc::ARRAY_ELEMENT:
    return "ARRAY_ELEMENT";
  case NullErrSrc::INVOKE_RETURN:
    return "INVOKE_RETURN";
  case NullErrSrc::CHECK_CAST:
    return "CHECK_CAST";
  default:
    return "";
  }
}

constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD_V1_3 =
    "$WrCheckParameter_V1_3";
constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD_V1_4 =
    "$WrCheckParameter_V1_4";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD_V1_3_PRE =
    "$WrCheckExpression_V1_3_";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD_V1_4_PRE =
    "$WrCheckExpression_V1_4_";

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

constexpr const char* NEW_CHECK_PARAM_NULL_SIGNATURE_V1_4 =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/"
    "Object;I)V";

constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE_V1_3_PRE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3_";
constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4_PRE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_4_";

constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE_POST =
    ":(Ljava/lang/Object;)V";

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
inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_3(
    const std::string& msg) {
  std::string wrapper_signature;
  wrapper_signature.append(NEW_CHECK_EXPR_NULL_SIGNATURE_V1_3_PRE);
  wrapper_signature.append(msg);
  wrapper_signature.append(NEW_CHECK_EXPR_NULL_SIGNATURE_POST);
  return static_cast<DexMethod*>(DexMethod::get_method(wrapper_signature));
}

inline DexMethod* kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_4(
    const std::string& msg) {
  std::string wrapper_signature;
  wrapper_signature.append(NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4_PRE);
  wrapper_signature.append(msg);
  wrapper_signature.append(NEW_CHECK_EXPR_NULL_SIGNATURE_POST);
  return static_cast<DexMethod*>(DexMethod::get_method(wrapper_signature));
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
  for (auto err : all_NullErrSrc) {
    method = kotlin_nullcheck_wrapper::
        kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_3(get_err_msg(err));
    if (method) {
      null_check_methods.emplace(method);
    }
    method = kotlin_nullcheck_wrapper::
        kotlin_jvm_internal_Intrinsics_WrCheckExpression_V1_4(get_err_msg(err));
    if (method) {
      null_check_methods.emplace(method);
    }
  }
  return null_check_methods;
}

inline NullErrSrc get_wrapper_code(IROpcode opcode) {
  switch (opcode) {
  case IOPCODE_LOAD_PARAM_OBJECT:
    return NullErrSrc::LOAD_PARAM;
  case OPCODE_AGET_OBJECT:
    return NullErrSrc::ARRAY_ELEMENT;
  case OPCODE_CONST:
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS:
    return NullErrSrc::CONST;
  case OPCODE_IGET_OBJECT:
    return NullErrSrc::INSTANCE_FIELD;
  case OPCODE_SGET_OBJECT:
    return NullErrSrc::STATIC_FIELD;
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_POLYMORPHIC:
  case OPCODE_INVOKE_CUSTOM:
  case OPCODE_INVOKE_INTERFACE:
    return NullErrSrc::INVOKE_RETURN;
  case OPCODE_CHECK_CAST:
    return NullErrSrc::CHECK_CAST;
  default:
    return NullErrSrc::UNKNOWN_SRC;
  }
}

} // namespace kotlin_nullcheck_wrapper
