/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinLambdaAnalyzer.h"

#include "ClassUtil.h"
#include "DexAccess.h"
#include "IRCode.h"
#include "TypeUtil.h"

namespace {

// Returns true if the class matches the structure of a Kotlin lambda (either
// non-desugared or D8 desugared).
bool matches_kotlin_lambda_pattern(const DexClass* cls) {
  if (const auto* super_cls = cls->get_super_class();
      super_cls == type::kotlin_jvm_internal_Lambda()) {
    if (!klass::maybe_non_d8_desugared_anonymous_class(cls)) {
      return false;
    }
  } else if (super_cls == type::java_lang_Object()) {
    if (!klass::maybe_d8_desugared_anonymous_class(cls)) {
      return false;
    }
  } else {
    return false;
  }
  const auto* intfs = cls->get_interfaces();
  if (intfs->size() != 1) {
    return false;
  }
  const auto* intf = intfs->at(0);
  return type::is_kotlin_function_interface(intf);
}

} // namespace

std::optional<KotlinLambdaAnalyzer> KotlinLambdaAnalyzer::for_class(
    const DexClass* cls) {
  return matches_kotlin_lambda_pattern(cls)
             ? std::optional(KotlinLambdaAnalyzer(cls))
             : std::nullopt;
}

bool KotlinLambdaAnalyzer::is_non_capturing() const {
  return m_cls->get_ifields().empty();
}

bool KotlinLambdaAnalyzer::is_trivial(size_t max_instructions) const {
  if (!is_non_capturing()) {
    return false;
  }
  const DexMethod* invoke = get_invoke_method();
  return invoke != nullptr &&
         invoke->get_code()->count_opcodes() <= max_instructions;
}

DexMethod* KotlinLambdaAnalyzer::get_invoke_method() const {
  DexMethod* result = nullptr;
  for (auto* method : m_cls->get_vmethods()) {
    if (method->get_name()->str() == "invoke" && is_public(method) &&
        !is_synthetic(method) && method->get_code() != nullptr) {
      if (result != nullptr) {
        // Multiple invoke methods found, ill-formed lambda.
        return nullptr;
      }
      result = method;
    }
  }
  return result;
}
