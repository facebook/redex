#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

# Compares the aapt d of an xml file with the known/expected value.
if [ "$#" -le 3 ]; then
    echo "aapt binary, apk, file path within apk, and expected file required"
    exit 1
fi
AAPT=$1
APK=$2
XML_PATH=$3
EXPECTED=$4

TMP_FILE=$(mktemp)
trap 'rm -r $TMP_FILE' EXIT

$AAPT d --values xmltree "$APK" "$XML_PATH" > "$TMP_FILE"
diff -u "$EXPECTED" "$TMP_FILE"
