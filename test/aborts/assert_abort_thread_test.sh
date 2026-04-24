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

OUT_TMP="$(mktemp "$(dirname "$SCRIPT")/test.out.XXXXXX")"
DEBUG_STDERR="$(mktemp -d "$(dirname "$SCRIPT")/stderr_debug.XXXXXX")/d"
trap 'rm -f "$OUT_TMP" "${DEBUG_STDERR}".*; rmdir "$(dirname "$DEBUG_STDERR")" 2>/dev/null' EXIT

# We are using pass-through mode for simplicity. The actual data
# does not matter
# shellcheck disable=SC2015
REDEX_STDERR_DEBUG="$DEBUG_STDERR" \
"$FB_REDEX" --redex-binary "$REDEX_BINARY" \
  --config /tmp/test.config \
  --outdir /tmp/test --dex-files /tmp/test.apk \
  --assert-abort-thread "This is an abort test." \
  >"$OUT_TMP" 2>&1 && exit 1 || true

# Record pre/post-tr size and pre-tr NUL count.
{
  echo "pre_tr_size=$(wc -c < "$OUT_TMP")"
  echo "pre_tr_nul=$(tr -cd '\0' < "$OUT_TMP" | wc -c)"
} > "${DEBUG_STDERR}.outtmp_meta"

# Strip NUL bytes so grep doesn't treat the output as binary.
tr -d '\0' < "$OUT_TMP" > "${OUT_TMP}.clean"
mv "${OUT_TMP}.clean" "$OUT_TMP"

echo "post_tr_size=$(wc -c < "$OUT_TMP")" >> "${DEBUG_STDERR}.outtmp_meta"

# Check for abort message.
grep 'This is an abort test.' "$OUT_TMP" || ( echo "Did not find abort message" ; exit 1 ; )

# Check for stack symbolication.

dump_debug() {
    echo "=== .outtmp_meta ==="
    if [ -f "${DEBUG_STDERR}.outtmp_meta" ]; then
      cat "${DEBUG_STDERR}.outtmp_meta"
    else
      echo "(no outtmp_meta file)"
    fi
    echo "=== .streams ==="
    if [ -f "${DEBUG_STDERR}.streams" ]; then
      cat "${DEBUG_STDERR}.streams"
    else
      echo "(no streams file)"
    fi
    echo "=== End debug ==="
}

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
      echo "=== Full symbolicated stack trace ==="
      cat "$OUT_TMP"
      echo "=== End stack trace ==="
      dump_debug
      exit 1
    fi
}

STACK_FUNCTION="  (anonymous namespace)::assert_abort(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>> const&, unsigned long)"
check_stack "$STACK_FUNCTION" 4

STACK_FILE="tools/redex-all/main.cpp:"
check_stack "$STACK_FILE" 7
