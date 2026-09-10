/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Test for KotlinLambdaDeduplicationPass with non-singleton lambdas.
// This test runs KotlinStatelessLambdaSingletonRemovalPass first to remove
// the INSTANCE singletons, then KotlinLambdaDeduplicationPass to deduplicate
// the resulting non-singleton lambdas via new-instance redirection.

class KotlinLambdaDedupNonSingletonTest {
  var result1: Int = 0
  var result2: Int = 0
  var result3: Int = 0
  var result4: Int = 0

  // Helper function that takes a lambda
  fun compute(fn: (Int, Int) -> Int): Int {
    return fn(100, 200)
  }

  // Group of 3 identical lambdas - meets threshold, should be deduplicated.
  // After KotlinStatelessLambdaSingletonRemovalPass removes the INSTANCE
  // singletons, these become non-singleton lambdas that are instantiated
  // each time. KotlinLambdaDeduplicationPass should then redirect the
  // new-instance calls to use the same canonical lambda class.
  fun useLambda1() {
    result1 = compute { a, b -> a + b }
  }

  fun useLambda2() {
    result2 = compute { a, b -> a + b }
  }

  fun useLambda3() {
    result3 = compute { a, b -> a + b }
  }

  // Second group of 4 identical lambdas - should be deduplicated to a
  // DIFFERENT canonical class than the first group.
  var secondGroupResult1: Int = 0
  var secondGroupResult2: Int = 0
  var secondGroupResult3: Int = 0
  var secondGroupResult4: Int = 0

  fun useSecondGroupLambda1() {
    secondGroupResult1 = compute { a, b -> a - b }
  }

  fun useSecondGroupLambda2() {
    secondGroupResult2 = compute { a, b -> a - b }
  }

  fun useSecondGroupLambda3() {
    secondGroupResult3 = compute { a, b -> a - b }
  }

  fun useSecondGroupLambda4() {
    secondGroupResult4 = compute { a, b -> a - b }
  }

  // Unique lambda (only 1 instance) - should NOT be deduplicated.
  fun useUniqueLambda() {
    result4 = compute { a, b -> a * b }
  }

  // Group of 2 identical lambdas - below min_duplicate_group_size threshold.
  // Should NOT be deduplicated.
  var belowThresholdResult1: Int = 0
  var belowThresholdResult2: Int = 0

  fun useBelowThresholdLambda1() {
    belowThresholdResult1 = compute { a, b -> a / b }
  }

  fun useBelowThresholdLambda2() {
    belowThresholdResult2 = compute { a, b -> a / b }
  }
}
