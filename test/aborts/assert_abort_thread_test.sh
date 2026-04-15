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
DEBUG_STDERR="$(dirname "$SCRIPT")/stderr_debug"

# We are using pass-through mode for simplicity. The actual data
# does not matter
# shellcheck disable=SC2015
REDEX_STDERR_DEBUG="$DEBUG_STDERR" \
"$FB_REDEX" --redex-binary "$REDEX_BINARY" \
  --config /tmp/test.config \
  --outdir /tmp/test --dex-files /tmp/test.apk \
  --assert-abort-thread "This is an abort test." \
  >"$OUT_TMP" 2>&1 && exit 1 || true

# Strip NUL bytes so grep doesn't treat the output as binary.
tr -d '\0' < "$OUT_TMP" > "${OUT_TMP}.clean"
mv "${OUT_TMP}.clean" "$OUT_TMP"

# Check for abort message.
grep 'This is an abort test.' "$OUT_TMP" || ( echo "Did not find abort message" ; exit 1 ; )

# Check for stack symbolication.

dump_debug() {
    echo "=== Raw pipe bytes (before Python decode/handler) ==="
    if [ -f "${DEBUG_STDERR}.raw" ]; then
      xxd "${DEBUG_STDERR}.raw" | head -200
      echo ""
      echo "--- terminate/what lines in raw pipe ---"
      grep -an 'terminate\|what()' "${DEBUG_STDERR}.raw" || echo "(none)"
      echo "--- NUL byte check ---"
      if tr -d '\0' < "${DEBUG_STDERR}.raw" | cmp -s - "${DEBUG_STDERR}.raw"; then
        echo "No NUL bytes in raw pipe"
      else
        echo "NUL bytes present in raw pipe"
      fi
    else
      echo "(no raw debug file)"
    fi
    echo "=== Python decode/handler metadata ==="
    if [ -f "${DEBUG_STDERR}.meta" ]; then
      cat "${DEBUG_STDERR}.meta"
    else
      echo "(no metadata file - no decode errors or handler mutations)"
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
