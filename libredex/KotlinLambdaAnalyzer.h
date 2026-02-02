/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include "DexClass.h"

/**
 * Analyzer for Kotlin lambda classes that provides efficient access to
 * lambda properties without redundant is_kotlin_lambda checks.
 *
 * Use the static for_class() factory to create an instance. Returns nullopt
 * if the class is not a Kotlin lambda. This makes for_class() suitable for
 * checking whether a class is a Kotlin lambda:
 *
 *   if (KotlinLambdaAnalyzer::for_class(cls)) {
 *     // cls is a Kotlin lambda
 *   }
 *
 * If you need to access lambda properties, capture the analyzer:
 *
 *   if (auto analyzer = KotlinLambdaAnalyzer::for_class(cls)) {
 *     if (analyzer->is_non_capturing()) {
 *       auto* invoke = analyzer->get_invoke_method();
 *       ...
 *     }
 *   }
 */
class KotlinLambdaAnalyzer final {
 public:
  KotlinLambdaAnalyzer() = delete;
  /**
   * Creates a KotlinLambdaAnalyzer if the class is a Kotlin lambda, or returns
   * nullopt otherwise. Can be used both to check if a class is a Kotlin lambda
   * and to analyze its properties.
   */
  [[nodiscard]] static std::optional<KotlinLambdaAnalyzer> for_class(
      const DexClass* cls);

  /**
   * Returns true if the lambda is non-capturing (no instance fields).
   */
  bool is_non_capturing() const;

  /**
   * Returns true if the lambda is trivial: non-capturing with an invoke
   * method of at most max_instructions instructions.
   *
   * The default of 4 instructions corresponds to a lambda with a single
   * statement, e.g., { true } compiles to:
   *
   *     const/4 v0, 0x1
   *     invoke-static {v0}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
   *     move-result-object v0
   *     return-object v0
   */
  bool is_trivial(size_t max_instructions = 4u) const;

  /**
   * Returns the invoke method of the lambda class, or nullptr if not found
   * or ill-formed (multiple invoke methods).
   */
  DexMethod* get_invoke_method() const;

  /**
   * Returns the underlying class.
   */
  const DexClass* get_class() const { return m_cls; }

 private:
  explicit KotlinLambdaAnalyzer(const DexClass* cls) : m_cls(cls) {}

  const DexClass* const m_cls;
};
