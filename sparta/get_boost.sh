#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BOOST_VERSION="1.71.0"

BOOST_VERSION_UNDERSCORE="${BOOST_VERSION//./_}"
BOOST_FILE="boost_${BOOST_VERSION_UNDERSCORE}.tar.bz2"
BOOST_TAR_URL="https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VERSION}/source/${BOOST_FILE}"
BOOST_DIR="boost_${BOOST_VERSION_UNDERSCORE}"

set -e

wget "$BOOST_TAR_URL" -O "$BOOST_FILE"
tar --bzip2 -xf "$BOOST_FILE"
cd "$BOOST_DIR"
./bootstrap.sh --with-libraries=thread
./b2 -d0 install
