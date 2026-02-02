/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinLambdaAnalyzer.h"

#include "DexAccess.h"
#include "IRCode.h"
#include "TypeUtil.h"

std::optional<KotlinLambdaAnalyzer> KotlinLambdaAnalyzer::for_class(
    const DexClass* cls) {
  return type::is_kotlin_lambda(cls) ? std::optional(KotlinLambdaAnalyzer(cls))
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
