#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

# Expects the executable and an .apk file, which will have its .arsc file
# extracted and inspected. This test is supposed to be a very minimal "does the
# tool compile and spew something" check, as a signal for automated deployments.
# The APIs are unit tested separately, with specially crafted input data.
if [ "$#" -le 1 ]; then
    echo "arsc_attribution binary, target apk required"
    exit 1
fi
ATTRIBUTION_BIN=$1
APK=$2

TMP_DIR=$(mktemp -d)
trap 'rm -r $TMP_DIR' EXIT

unzip "$APK" resources.arsc -d "$TMP_DIR"
ARSC_FILE="$TMP_DIR/resources.arsc"
CSV_FILE="$TMP_DIR/out.csv"

echo "Running binary"
pushd "$TMP_DIR"
"$ATTRIBUTION_BIN" --file "$ARSC_FILE" > out.csv
popd

# Just check that some resource ID was spit out.
grep -q "^0x7f010000.*default$" "$CSV_FILE"
