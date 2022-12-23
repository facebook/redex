#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

# Take a zip file path, and a variable number of file paths that should be
# tested to make sure they are not entries in the zip.
if [ "$#" -le 1 ]; then
    echo "Zip and target file(s) required"
    exit 1
fi
ZIP_FILE=$1
shift
TMP_FILE=$(mktemp)
unzip -l "$ZIP_FILE" > "$TMP_FILE"
while test $# -gt 0
do
    if grep -q "$1" "$TMP_FILE"; then
      echo "File $1 was found in zip and should not be there." >&2
      echo "" >&2
      cat "$TMP_FILE" >&2
      exit 1
    fi
    shift
done
