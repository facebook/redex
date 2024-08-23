#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

SCRIPT="$0"

FB_REDEX="$1"
shift
REDEX_BINARY="$1"
shift

OUT_TMP="$(dirname "$SCRIPT")/test.out"

# We are using pass-through mode for simplicity. The actual data
# does not matter
# shellcheck disable=SC2015
"$FB_REDEX" --redex-binary "$REDEX_BINARY" \
  --outdir /tmp/test --dex-files /tmp/test.apk \
  --assert-abort "This is an abort test." \
  >"$OUT_TMP" 2>&1 && exit 1 || true

# Check for abort message.
grep 'This is an abort test.' "$OUT_TMP" || ( echo "Did not find abort message" ; exit 1 ; )

# Check for stack symbolication.

check_stack() {
    LINE="$1"
    shift
    EXPECTED_COUNT="$1"
    shift

    # Does it exist at all?
    grep "$LINE" "$OUT_TMP" >/dev/null || ( echo "Did not find $LINE" ; exit 1 ; )

    # Does it exist the right amount of times?
    COUNT=$(grep -c "$LINE" "$OUT_TMP")
    if [ "$COUNT" -ne "$EXPECTED_COUNT" ] ;  then
      echo "Found $LINE $COUNT times, expected $EXPECTED_COUNT times!"
      exit 1
    fi
}

STACK_FUNCTION="  (anonymous namespace)::assert_abort(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>> const&, unsigned long)"
check_stack "$STACK_FUNCTION" 4

STACK_FILE="tools/redex-all/main.cpp:"
check_stack "$STACK_FILE" 8
