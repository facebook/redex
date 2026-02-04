/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Test for KotlinLambdaDeduplicationPass.
// This test creates multiple lambdas with identical invoke code that
// should be deduplicated by the pass.

class KotlinLambdaDeduplicationTest {
  var result1: Int = 0
  var result2: Int = 0
  var result3: Int = 0
  var result4: Int = 0

  // Helper function that takes a lambda
  fun compute(fn: (Int, Int) -> Int): Int {
    return fn(100, 200)
  }

  // Group of 3 identical trivial lambdas - meets threshold, should be deduplicated.
  fun useTrivialLambda1() {
    result1 = compute { a, b -> a + b }
  }

  fun useTrivialLambda2() {
    result2 = compute { a, b -> a + b }
  }

  fun useTrivialLambda3() {
    result3 = compute { a, b -> a + b }
  }

  // Group of 4 identical trivial lambdas - above threshold, should be deduplicated
  // to a DIFFERENT holder field than the group above.
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

  // Group of 3 identical non-trivial lambdas - exceeds trivial_lambda_max_instructions.
  // Should NOT be deduplicated despite meeting the group size threshold.
  var nonTrivialResult1: Int = 0
  var nonTrivialResult2: Int = 0
  var nonTrivialResult3: Int = 0

  fun useNonTrivialLambda1() {
    nonTrivialResult1 = compute { a, b ->
      val temp1 = a + b
      val temp2 = temp1 * 2
      val temp3 = temp2 - a
      val temp4 = temp3 + b
      val temp5 = temp4 / 2
      val temp6 = temp5 + a
      val temp7 = temp6 - b
      val temp8 = temp7 * 3
      val temp9 = temp8 + a
      val temp10 = temp9 - b
      temp10 / 2
    }
  }

  fun useNonTrivialLambda2() {
    nonTrivialResult2 = compute { a, b ->
      val temp1 = a + b
      val temp2 = temp1 * 2
      val temp3 = temp2 - a
      val temp4 = temp3 + b
      val temp5 = temp4 / 2
      val temp6 = temp5 + a
      val temp7 = temp6 - b
      val temp8 = temp7 * 3
      val temp9 = temp8 + a
      val temp10 = temp9 - b
      temp10 / 2
    }
  }

  fun useNonTrivialLambda3() {
    nonTrivialResult3 = compute { a, b ->
      val temp1 = a + b
      val temp2 = temp1 * 2
      val temp3 = temp2 - a
      val temp4 = temp3 + b
      val temp5 = temp4 / 2
      val temp6 = temp5 + a
      val temp7 = temp6 - b
      val temp8 = temp7 * 3
      val temp9 = temp8 + a
      val temp10 = temp9 - b
      temp10 / 2
    }
  }
}
