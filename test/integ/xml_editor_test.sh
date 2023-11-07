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

# Adds attributes to nodes
echo "Adding an attribute to uses-sdk"
"$EDITOR_BIN" "$MANIFEST" uses-sdk testAttribute 0x300 0x01010271 0x11 android
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:testAttribute(0x01010271)=(type 0x11)0x300"
echo "successfully added an attribute"

echo "Adding a class attribute to manifest"
"$EDITOR_BIN" "$MANIFEST" manifest class classString 0x0101028d 0x03 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:class(0x0101028d)=\"classString\" (Raw: \"classString\")"
echo "successfully added an attribute"

echo "Adding an attribute to manifest"
"$EDITOR_BIN" "$MANIFEST" manifest anotherTest testString 0x0101021d 0x03 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:anotherTest(0x0101021d)=\"testString\" (Raw: \"testString\")"
echo "successfully added an attribute"

echo "Adding an attribute to application"
"$EDITOR_BIN" "$MANIFEST" application noIDAttribute 0x500 0 0x11 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:noIDAttribute=(type 0x11)0x500"
echo "successfully added an attribute"

echo "Adding an attribute to receiver"
"$EDITOR_BIN" "$MANIFEST" receiver noNamespaceAttribute 0x600 0x01010007 0x11 "" ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "noNamespaceAttribute(0x01010007)=(type 0x11)0x600"
echo "successfully added an attribute"

echo "Adding an attribute to application with the same name+id as an existing attribute"
"$EDITOR_BIN" "$MANIFEST" application noNamespaceAttribute 0x700 0x01010007 0x11
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "noNamespaceAttribute(0x01010007)=(type 0x11)0x700"
echo "successfully added an attribute"

echo "Adding an attribute and node to manifest"
"$EDITOR_BIN" "$MANIFEST" manifest newAttribute 0x700 0x01010011 0x11 android testNode
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "testNode"
$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:newAttribute(0x01010011)=(type 0x11)0x700"
echo "successfully added an attribute"

echo "Using the long argument version to edit an attribute"
"$EDITOR_BIN" "$MANIFEST" uses-sdk minSdkVersion 0x14 0x0101020c 0x10 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

COUNT=$($AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -c "android:minSdkVersion")
if [[ $COUNT -ne 1 ]]
then
  echo "Incorrect number of minSdkVersion attributes, expected 1"
  # exit the script to fail the test
  exit 1
fi
echo "successfully edited a minSdkVersion"

echo "Adding an attribute with a large id"
"$EDITOR_BIN" "$MANIFEST" manifest largeIDAttribute 0x250 0x31010011 0x11 android
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:largeIDAttribute(0x31010011)=(type 0x11)0x250"
echo "successfully added an attribute"

echo "Reset a string value of an existing attribute"
"$EDITOR_BIN" "$MANIFEST" manifest package "one two three" 0 0x3 ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "package=\"one two three\" (Raw: \"one two three\")"
echo "successfully added an attribute"

echo "Adding an atttibute without an id but with the same name as an existing attribute"
"$EDITOR_BIN" "$MANIFEST" manifest noIDAttribute 0x800 0 0x11 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:noIDAttribute=(type 0x11)0x800"
echo "successfully added an attribute"

echo "Editing an attribute should only edit the first instance of it"
"$EDITOR_BIN" "$MANIFEST" activity name newName 0x01010003 0x03 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:name(0x01010003)=\"newName\" (Raw: \"newName\")"
COUNT=$($AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q -c ".PublicActivity")
if [[ $COUNT -ne 0 ]]
then
  echo "There should be no attributes named .PublicActivity"
  # exit the script to fail the test
  exit 1
fi
echo "successfully edited an attribute"

echo "Adding an attribute should only add it to the first matching node"
"$EDITOR_BIN" "$MANIFEST" activity noIDAttribute 0x900 0 0x11 android ""
pushd "$TMP_DIR"
zip test_ver.zip AndroidManifest.xml
popd

$AAPT d --values xmltree "$TMP_DIR/test_ver.zip" AndroidManifest.xml | grep -q "android:noIDAttribute=(type 0x11)0x900"
echo "successfully added an attribute"

rm -rf "$TMP_DIR"
