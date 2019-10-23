#!/bin/bash -x
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

pushd test
test -e gtest-1.7.0.zip || {
    curl https://codeload.github.com/google/googletest/zip/release-1.7.0 \
         -o gtest-1.7.0.zip
    unzip -o gtest-1.7.0.zip
}
popd
