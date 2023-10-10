#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

# Expects the executable and an .apk file, which will have its manifest
# attribute values rewritten to new values
if [ "$#" -le 2 ]; then
    echo "xml_editor binary, target apk and aapt binary required"
    exit 1
fi
EDITOR_BIN=$1
APK=$2
AAPT=$3
TMP_DIR=$(mktemp -d)

unzip "$APK" AndroidManifest.xml -d "$TMP_DIR"
MANIFEST="$TMP_DIR/AndroidManifest.xml"

echo "Rewriting package attribute"
"$EDITOR_BIN" "$MANIFEST" manifest package com.facebook.bananas
pushd "$TMP_DIR"
zip test_str.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_str.zip" AndroidManifest.xml | grep -q "package=\"com.facebook.bananas\""
echo "package attribute was rewritten successfully"

echo "Rewriting versionCode"
"$EDITOR_BIN" "$MANIFEST" manifest 0x0101021b 0x200
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q -E "android:versionCode.*0x200$"
echo "versionCode was rewritten successfully"

rm -rf "$TMP_DIR"
