#!/bin/bash -x

set -e

pushd test
test -e gtest-1.7.0.zip || {
    curl -O https://googletest.googlecode.com/files/gtest-1.7.0.zip
    unzip -o gtest-1.7.0.zip
}
popd
