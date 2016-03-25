#!/bin/bash

ROOT=$((git rev-parse --show-toplevel || hg root) 2>/dev/null)
REDEX=$ROOT/fbandroid/native/redex/redex.py
if [ ! -f "$REDEX" ]; then
    REDEX="$ROOT/native/redex/redex.py"
    if [ ! -f "$REDEX" ]; then
        echo "Couldn't find redex"
        exit 1
    fi
fi

if [ $# != 2 ]; then
    echo "Usage: compare_bits.sh apk1 apk2"
    exit 1
fi

DEX1=$($REDEX -u $1 2>&1 | grep ^DEX | cut -d' ' -f 2)
DEX2=$($REDEX -u $2 2>&1 | grep ^DEX | cut -d' ' -f 2)

diff -r $DEX1 $DEX2
