#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

REDEX=$(dirname $0)/../redex.py
if [ ! -f "$REDEX" ]; then
    echo "Couldn't find redex"
    exit 1
fi

if [ $# != 2 ]; then
    echo "Usage: compare_bits.sh apk1 apk2"
    exit 1
fi

DEX1=$($REDEX -u $1 2>&1 | grep ^DEX | cut -d' ' -f 2)
DEX2=$($REDEX -u $2 2>&1 | grep ^DEX | cut -d' ' -f 2)

diff -r $DEX1 $DEX2
