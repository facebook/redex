#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Set-up the dependencies necessary to build and run Redex on Ubuntu and Debian,
# using APT for software management.

# Exit on any command failing
set -e

# Root directory of repository
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

# Temporary directory for toolchain sources. Build artifacts will be
# installed to /usr/local.
echo "toolchain tmp = $TOOLCHAIN_TMP"
if [ -z "$TOOLCHAIN_TMP" ] ; then
  TOOLCHAIN_TMP=$(mktemp -d 2>/dev/null)
  trap 'rm -r $TOOLCHAIN_TMP' EXIT
else
  echo "Using toolchain tmp $TOOLCHAIN_TMP"
  mkdir -p "$TOOLCHAIN_TMP"
fi

DEB_UBUNTU_PKGS="unzip"

BOOST_DEB_UBUNTU_PKGS="libboost-filesystem-dev
                       libboost-iostreams-dev
                       libboost-program-options-dev
                       libboost-regex-dev
                       libboost-system-dev
                       libboost-thread-dev

PROTOBUF_DEB_UBUNTU_PKGS="libprotobuf-dev
                          protobuf-compiler"

function install_googletest_from_source {
    pushd "$TOOLCHAIN_TMP"
    mkdir -p dl_cache/gtest
    GOOGLETEST_MIN_VERSION=1.14.0
    if [ ! -f "dl_cache/gtest/googletest-${GOOGLETEST_MIN_VERSION}.tar.gz" ] ; then
        wget  "https://codeload.github.com/google/googletest/tar.gz/v${GOOGLETEST_MIN_VERSION}" -O "dl_cache/gtest/googletest-${GOOGLETEST_MIN_VERSION}.tar.gz"
    fi
    mkdir -p toolchain_install/gtest
    pushd toolchain_install/gtest
    tar xf "../../dl_cache/gtest/googletest-${GOOGLETEST_MIN_VERSION}.tar.gz" --no-same-owner --strip-components=1
    cmake .
    cmake --build . --target install
    popd
    popd
}

function install_kotlin_from_source {
    pushd "$TOOLCHAIN_TMP"
    mkdir -p dl_cache/kotlin
    KOTLIN_VERSION=1.3.31
    if [ ! -f "dl_cache/kotlin/kotlin-compiler-${KOTLIN_VERSION}.zip" ] ; then
        wget "https://github.com/JetBrains/kotlin/releases/download/v${KOTLIN_VERSION}/kotlin-compiler-${KOTLIN_VERSION}.zip" -O "dl_cache/kotlin/kotlin-compiler-${KOTLIN_VERSION}.zip"
    fi
    mkdir -p toolchain_install/kotlin
    pushd toolchain_install/kotlin
    unzip "../../dl_cache/kotlin/kotlin-compiler-${KOTLIN_VERSION}.zip"
    cp -v kotlinc/bin/* /usr/local/bin
    cp -v kotlinc/lib/* /usr/local/lib
    popd
    popd
}

function install_from_apt {
  PKGS="autoconf
        autoconf-archive
        automake
        binutils-dev
        bzip2
        ca-certificates
        cmake
        g++
        libiberty-dev
        libjemalloc-dev
        libjsoncpp-dev
        liblz4-dev
        liblzma-dev
        libtool
        make
        wget
        zlib1g-dev $*"
  apt-get update -q
  apt-get install -q --no-install-recommends -y ${PKGS}
}

function handle_debian {
    case $1 in
        [1-9]|10)
            echo "Unsupported Debian version $1"
            exit 1
            ;;
        11)
            install_from_apt  python3 ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            install_kotlin_from_source
            ;;
        *)
            install_from_apt  python3 kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            ;;
    esac
    # TODO(T227009978): Install googletest from apt for some Debian versions after enabling autodetecting googletest installation dir.
    install_googletest_from_source
}

function handle_ubuntu {
    case $1 in
        2*)
            install_from_apt python3 kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            ;;
        *)
            echo "Unsupported Ubuntu version $1"
            exit 1
            ;;
    esac
    # TODO(T227009978): Install googletest from apt for some Ubuntu versions after enabling autodetecting googletest installation dir.
    install_googletest_from_source
}

# Read ID and VERSION_ID from /etc/os-release.
declare $(grep -E '^(ID|VERSION_ID)=' /etc/os-release | xargs)

case $ID in
ubuntu)
    handle_ubuntu "$VERSION_ID"
    ;;
debian)
    handle_debian "$VERSION_ID"
    ;;
*)
    echo "Unsupported OS $ID - $VERSION_ID"
    exit 1
esac
