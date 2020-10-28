#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

for test in *_test; do
  if [[ -f "$test" ]]; then
    ./"$test"
  else
    # Wildcard did not expand into any file (i.e. files don't exist)
    echo "Tests not found. Please make sure you are in the build directory."
    exit 2
  fi
done
