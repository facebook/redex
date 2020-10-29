#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

N_TEST_FAILED=0
FAILED_TESTS=$""

failed_test() {
  ((N_TEST_FAILED++))
  FAILED_TESTS=$(printf "%s\n%s" "$FAILED_TESTS" "$1")
}

for test in *_test; do
  if [[ -f "$test" ]]; then
    ./"$test" || failed_test "$test"
  else
    # Wildcard did not expand into any file (i.e. files don't exist)
    echo "Tests not found. Please make sure you are in the build directory."
    exit 2
  fi
done

if [[ $N_TEST_FAILED -ne 0 ]]; then
  printf "%s test(s) failed.%s\n" "$N_TEST_FAILED" "$FAILED_TESTS"
  exit 1
fi
