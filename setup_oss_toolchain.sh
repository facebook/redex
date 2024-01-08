#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Set-up the dependencies necessary to build and run Redex on Ubuntu 16.04
# Xenial, using APT for software management.

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

function install_boost_from_source {
    pushd "$TOOLCHAIN_TMP"
    "$ROOT"/get_boost.sh
}

function install_protobuf3_from_source {
    pushd "$TOOLCHAIN_TMP"
    mkdir -p dl_cache/protobuf
    if [ ! -f dl_cache/protobuf/protobuf-cpp-3.17.3.tar.gz ] ; then
      wget https://github.com/protocolbuffers/protobuf/releases/download/v3.17.3/protobuf-cpp-3.17.3.tar.gz -O dl_cache/protobuf/protobuf-cpp-3.17.3.tar.gz
    fi

    mkdir -p toolchain_install/protobuf
    pushd toolchain_install/protobuf
    tar -xvf ../../dl_cache/protobuf/protobuf-cpp-3.17.3.tar.gz --no-same-owner

    pushd protobuf-3.17.3
    ./configure $BITNESS_CONFIGURE
    make -j 4 V=0 && make install V=0
}

function install_from_apt {
  PKGS="autoconf
        autoconf-archive
        automake
        binutils-dev
        bzip2
        ca-certificates
        g++
        libiberty-dev$BITNESS_SUFFIX
        libjemalloc-dev$BITNESS_SUFFIX
        libjsoncpp-dev$BITNESS_SUFFIX
        liblz4-dev$BITNESS_SUFFIX
        liblzma-dev$BITNESS_SUFFIX
        libtool
        make
        wget
        zlib1g-dev$BITNESS_SUFFIX $BITNESS_PKGS $*"
  apt-get update
  apt-get install --no-install-recommends -y ${PKGS}
}

function handle_debian {
    case $1 in
        [1-9])
            echo "Unsupported Debian version $1"
            exit 1
            ;;
        10)
            if [ "$BITNESS" == "32" ] ; then
                echo "32-bit compile unsupported because of boost"
                exit 1
            fi
            install_from_apt python3 ${DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            install_boost_from_source
            ;;
        *)
            install_from_apt  python3 ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
            ;;
    esac
}

function handle_ubuntu {
    case $1 in
        1[7-9]*)
            if [ "$BITNESS" == "32" ] ; then
                echo "32-bit compile unsupported because of boost"
                exit 1
            fi
            install_from_apt python3 ${DEB_UBUNTU_PKGS}
            install_boost_from_source
            install_protobuf3_from_source
            ;;
        2*)
            install_from_apt python3 ${DEB_UBUNTU_PKGS} ${BOOST_DEB_UBUNTU_PKGS} ${PROTOBUF_DEB_UBUNTU_PKGS}
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
