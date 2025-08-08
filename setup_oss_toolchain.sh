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

if [ "$1" = "32" ] ; then
  dpkg --add-architecture i386

  BITNESS="32"
  BITNESS_SUFFIX=":i386"
  BITNESS_CONFIGURE="--host=i686-linux-gnu CFLAGS=-m32 CXXFLAGS=-m32 LDFLAGS=-m32"
  BITNESS_PKGS="gcc-multilib g++-multilib"

  echo "Use --host=i686-linux-gnu CFLAGS=-m32 CXXFLAGS=-m32 LDFLAGS=-m32 for ./configure"
else
  BITNESS="64"  # Assumption here, really means host-preferred arch.
  BITNESS_SUFFIX=":"
  BITNESS_CONFIGURE=""
  BITNESS_PKGS=""
fi

DEB_UBUNTU_PKGS="unzip"

BOOST_DEB_UBUNTU_PKGS="libboost-filesystem-dev$BITNESS_SUFFIX
                       libboost-iostreams-dev$BITNESS_SUFFIX
                       libboost-program-options-dev$BITNESS_SUFFIX
                       libboost-regex-dev$BITNESS_SUFFIX
                       libboost-system-dev$BITNESS_SUFFIX
                       libboost-thread-dev$BITNESS_SUFFIX"

PROTOBUF_DEB_UBUNTU_PKGS="libprotobuf-dev$BITNESS_SUFFIX
                          protobuf-compiler"

GOOGLETEST_DEB_UBUNTU_PKGS="libgtest-dev$BITNESS_SUFFIX
                            libgmock-dev$BITNESS_SUFFIX"

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
    # GoogleTest's string_view matcher requires compiler to support C++17 or
    # later. Older GCC versions need to be explicitly told to use a later C++
    # standard.
    if [ "$BITNESS" = "32" ] ; then
        CFLAGS=-m32 CXXFLAGS="-m32 -std=gnu++20" LDFLAGS=-m32 cmake .
    else
        CXXFLAGS="-std=gnu++20" cmake .
    fi
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
        libiberty-dev$BITNESS_SUFFIX
        libjemalloc-dev$BITNESS_SUFFIX
        libjsoncpp-dev$BITNESS_SUFFIX
        liblz4-dev$BITNESS_SUFFIX
        liblzma-dev$BITNESS_SUFFIX
        libtool
        make
        python3
        wget
        zlib1g-dev$BITNESS_SUFFIX $BITNESS_PKGS $*"
  apt-get update -q
  apt-get install -q --no-install-recommends -y ${PKGS}
}

function handle_debian {
    case $1 in
        1[3-9])
            install_from_apt default-jdk-headless kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS} ${GOOGLETEST_DEB_UBUNTU_PKGS}
            ;;
        12)
            install_from_apt default-jdk-headless kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            install_googletest_from_source
            ;;
        11)
            install_from_apt default-jdk-headless ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            install_kotlin_from_source
            install_googletest_from_source
            ;;
        *)
            echo "Unsupported Debian version $1"
            exit 1
            ;;
    esac
}

function handle_ubuntu {
    case $1 in
        2[4-9]*)
            # We don't support JDK 21 yet. Replace this with default-jdk-headless once we support it.
            install_from_apt openjdk-17-jdk-headless kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS} ${GOOGLETEST_DEB_UBUNTU_PKGS}
            ;;
        2[2-3]*)
            install_from_apt default-jdk-headless kotlin ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            install_googletest_from_source
            ;;
        *)
            echo "Unsupported Ubuntu version $1"
            exit 1
            ;;
    esac
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
