/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string_view>

#include "Pass.h"

class KotlinTrivialLambdaDeduplicationPass : public Pass {
 public:
  // Name used for canonical INSTANCE fields after deduplication.
  // Not named "INSTANCE" to prevent KotlinStatelessLambdaSingletonRemovalPass
  // from inlining the singleton access.
  static constexpr std::string_view kDedupedInstanceName =
      "INSTANCE$redex$dedup";

  KotlinTrivialLambdaDeduplicationPass()
      : Pass("KotlinTrivialLambdaDeduplicationPass") {}

  // This pass can only run once per Redex invocation. We require the
  // TrivialKotlinLambdasNotDeduplicated property (initially established) and
  // destroy it after running. Other passes preserve it by default. If the pass
  // is not configured, the property remains established (no error since it's
  // not a negative/must-not property).
  //
  // We require DexLimitsObeyed to ensure this pass runs after InterDex. This
  // allows us to pick the canonical lambda from the lowest-indexed dex file
  // (e.g., classes.dex < classes2.dex) so that higher-indexed dexes can
  // reference it without creating illegal cross-dex references.
  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {TrivialKotlinLambdasNotDeduplicated, Requires},
        {DexLimitsObeyed, RequiresAndPreserves},
    };
  }

  std::string get_config_doc() override {
    return R"(
This pass deduplicates trivial Kotlin lambdas that have identical code.

A trivial lambda is a non-capturing lambda (no instance fields) whose invoke
method has a small number of instructions (configurable, default <= 4).

This pass is effective for very common lambdas like { true } or { null } that
are often duplicated across many call sites in an app. Each duplicate lambda
generates a separate class, and deduplicating them reduces code size and the
number of classes.

For lambdas with identical invoke code, this pass:
1. Picks the canonical lambda from the lowest-indexed dex file (e.g.,
   classes.dex < classes2.dex) so higher-indexed dexes can reference it
2. Renames the canonical's INSTANCE field to prevent later passes from
   inlining it
3. Rewrites all usages of duplicate lambda INSTANCEs to use the canonical's
   INSTANCE
    )";
  }

  void bind_config() override {
    bind("trivial_lambda_max_instructions",
         kDefaultTrivialLambdaMaxInstructions,
         m_trivial_lambda_max_instructions,
         "Maximum number of instructions for a lambda to be considered "
         "trivial");
    bind("min_duplicate_group_size",
         kDefaultMinDuplicateGroupSize,
         m_min_duplicate_group_size,
         "Minimum number of lambdas with identical code required to form a "
         "duplicate group for deduplication");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  static constexpr size_t kDefaultTrivialLambdaMaxInstructions = 4;
  // If a lambda is not deduped, KotlinStatelessLambdaSingletonRemovalPass
  // rewrites each of its usages with 3 instructions. Slightly more than 4 (5
  // here) may be a good default to start with.
  static constexpr size_t kDefaultMinDuplicateGroupSize = 5;

  size_t m_trivial_lambda_max_instructions{
      kDefaultTrivialLambdaMaxInstructions};
  size_t m_min_duplicate_group_size{kDefaultMinDuplicateGroupSize};
};
