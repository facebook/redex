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

# Temporary directory for toolchain sources. Build artifacts will be
# installed to /usr/local.
TMP=$(mktemp -d 2>/dev/null)
trap 'rm -r $TMP' EXIT

BOOST_DEB_UBUNTU_PKGS="libboost-filesystem-dev
                       libboost-iostreams-dev
                       libboost-program-options-dev
                       libboost-regex-dev
                       libboost-system-dev
                       libboost-thread-dev"

function install_python36_from_source {
    pushd "$TMP"
    wget https://www.python.org/ftp/python/3.6.10/Python-3.6.10.tgz
    tar -xvf Python-3.6.10.tgz
    pushd Python-3.6.10

    ./configure
    make V=0 && make install V=0
}

function install_boost_from_source {
    pushd "$TMP"
    "$ROOT"/get_boost.sh
}

function install_from_apt {
  PKGS="autoconf
        autoconf-archive
        automake
        binutils-dev
        bzip2
        ca-certificates
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
            install_from_apt python3
            install_boost_from_source
            ;;
        *)
            install_from_apt ${BOOST_DEB_UBUNTU_PKGS} python3
            ;;
    esac
}

function handle_ubuntu {
    case $1 in
        16*)
            install_from_apt
            install_python36_from_source
            install_boost_from_source
            ;;
        1[7-9]*)
            install_from_apt python3
            install_boost_from_source
            ;;
        2*)
            install_from_apt ${BOOST_DEB_UBUNTU_PKGS} python3
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
