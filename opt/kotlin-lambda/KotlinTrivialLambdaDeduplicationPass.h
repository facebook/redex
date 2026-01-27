/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class KotlinTrivialLambdaDeduplicationPass : public Pass {
 public:
  KotlinTrivialLambdaDeduplicationPass()
      : Pass("KotlinTrivialLambdaDeduplicationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
    };
  }

  std::string get_config_doc() override {
    return R"(
This pass deduplicates trivial Kotlin lambdas that have identical code.

A trivial lambda is a non-capturing lambda (no instance fields) whose invoke
method has a small number of instructions (configurable, default <= 4).

For lambdas with identical invoke code, this pass:
1. Picks one canonical lambda class
2. Creates a holder class with a static INSTANCE field pointing to the
   canonical lambda's singleton
3. Rewrites all usages of duplicate lambda INSTANCEs to use the canonical one
4. Removes the duplicate lambda classes

This optimization reduces code size by eliminating redundant lambda classes.
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
