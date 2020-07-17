#!/usr/bin/env bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Set-up the dependencies necessary to build and run Redex on Ubuntu 16.04
# Xenial, using APT for software management.

# Exit on any command failing
set -e

# Root directory of repository
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

apt-get update
apt-get install -y \
    autoconf \
    autoconf-archive \
    automake \
    binutils-dev \
    bzip2 \
    g++ \
    g++-5 \
    libiberty-dev \
    libjemalloc-dev \
    libjsoncpp-dev \
    liblz4-dev \
    liblzma-dev \
    libtool \
    make \
    wget \
    zlib1g-dev

# Temporary directory for toolchain sources. Build artifacts will be
# installed to /usr/local.
TMP=$(mktemp -d 2>/dev/null)
trap 'rm -r $TMP' EXIT
pushd "$TMP"

# Python 3.6.

wget https://www.python.org/ftp/python/3.6.10/Python-3.6.10.tgz
tar -xvf Python-3.6.10.tgz
pushd Python-3.6.10

./configure
make && make install

popd

# Boost.

"$ROOT"/get_boost.sh
