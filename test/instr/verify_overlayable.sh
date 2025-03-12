#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

# Expects the executable and an .apk file, which will have its manifest
# attribute values rewritten to new values
if [ "$#" -le 1 ]; then
    echo "aapt binary and apk required"
    exit 1
fi
AAPT2=$1
APK=$2
TMP_FILE=$(mktemp)

$AAPT2 d resources "$APK" | grep "^ *resource.*OVERLAYABLE$" > "$TMP_FILE"

# For all given arguments beyond the first two, assert that the aapt2 command
# appears to have spit out a resource of that name that is OVERLAYABLE.
for arg in "${@:3}"; do
    COUNT=$(grep -c "$arg OVERLAYABLE$" "$TMP_FILE" || true)
    if [[ $COUNT -ne 1 ]]
    then
        cat "$TMP_FILE"
        echo "    ^^^^ There should be an overlayable resource named '$arg' ^^^^"
        rm -rf "$TMP_FILE"
        # exit the script to fail the test
        exit 1
    fi
done
rm -rf "$TMP_FILE"
