#! /bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# XXX(jezng): I tried running these commands as part of the Buck build, but I'm
# having trouble getting the Buck-given versions of AAPT and the SDK libraries
# to work together -- in particular, running the AAPT from v23 against the SDK
# from v26 causes AAPT to crash. Rather than spending time figuring out how to
# get Buck to use the versions I want, I'm just going to run the command
# locally and commit the result...

SCRIPT_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")

for SRC in $(find "$SCRIPT_DIR" -type f -name "*.xml"); do
  TMP=$(mktemp -d)
  MANIFEST="$TMP/AndroidManifest.xml"
  cp "$SRC" "$MANIFEST"
  /opt/android_sdk/build-tools/23.0.1/aapt package -M "$MANIFEST" -F "$TMP/out.apk" -I /opt/android_sdk/platforms/android-25/android.jar
  unzip "$TMP/out.apk" -d "$TMP/out"
  mv "$TMP/out/AndroidManifest.xml" "$SRC.bin"
  rm -rf "$TMP"
done
