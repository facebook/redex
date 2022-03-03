#!/bin/bash -x
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
pushd "$TEST_DIR"

# Note: Use at least 1.8 as we need googlemock, too.

test -e gtest-1.8.0.zip || {
    curl https://codeload.github.com/google/googletest/zip/release-1.8.0 \
         -o gtest-1.8.0.zip
    unzip -o gtest-1.8.0.zip
}
