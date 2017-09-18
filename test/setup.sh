#!/bin/bash -x

set -e

pushd test
test -e gtest-1.7.0.zip || {
    curl https://codeload.github.com/google/googletest/zip/release-1.7.0 \
         -o gtest-1.7.0.zip
    unzip -o gtest-1.7.0.zip
}
popd
