#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

SCRIPT="$0"

REDEX_BINARY="$1"
shift

OUT_FILE="$(dirname "$SCRIPT")/test.out"

# We need to give an input parameter, but it does not matter.
# shellcheck disable=SC2015
"$REDEX_BINARY" --asan-abort test.dex >"$OUT_FILE" 2>&1 && ( echo "Redex did not fail" ; exit 1 ; )

# Check for asan message.
grep 'ERROR: AddressSanitizer: stack-use-after-scope' "$OUT_FILE" || ( echo "Did not find ASAN header" ; exit 1 ; )
grep 'WRITE of size 4 ' "$OUT_FILE" || ( echo "Did not find write message" ; exit 1 ; )
grep '==ABORTING' "$OUT_FILE" || ( echo "Did not find abort message" ; exit 1 ; )
