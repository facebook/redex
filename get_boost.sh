#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BOOST_VERSION="1.71.0"

BOOST_VERSION_UNDERSCORE="${BOOST_VERSION//./_}"
BOOST_FILE="boost_${BOOST_VERSION_UNDERSCORE}.tar.bz2"
BOOST_TAR_URL="https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VERSION}/source/${BOOST_FILE}"
BOOST_CACHE_DIR="dl_cache/boost_cache"
BOOST_TAR_LOCAL="${BOOST_CACHE_DIR}/${BOOST_FILE}"
BOOST_DIR="boost_${BOOST_VERSION_UNDERSCORE}"

set -e

# Check for cached artifacts.
if [ ! -d "$BOOST_CACHE_DIR" ] ; then
  mkdir -p "$BOOST_CACHE_DIR"
fi
if [ ! -f "$BOOST_TAR_LOCAL" ] ; then
  echo "Downloading Boost 1.71.0"
  wget "$BOOST_TAR_URL" -O "$BOOST_TAR_LOCAL"
fi

mkdir -p toolchain_install/boost
pushd toolchain_install/boost

tar --bzip2 -xf "../../$BOOST_TAR_LOCAL"

cd "$BOOST_DIR"
./bootstrap.sh --with-libraries=filesystem,iostreams,program_options,regex,system,thread
./b2 -j 4 -d0 install
