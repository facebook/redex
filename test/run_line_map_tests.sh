#! /bin/bash

set -e
set -x

REDEX_ROOT=native/$(basename "$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )")

buck install $REDEX_ROOT/test/line_map:test_redex
buck build $REDEX_ROOT:line_remap
REMAPPER=`buck targets --show-output $REDEX_ROOT:line_remap | cut -d' ' -f2`

DIR="$(dirname "${BASH_SOURCE[0]}")"
# cd to fbandroid
cd $DIR/../../..

adb shell am instrument -w \
com.facebook.redex.test.line_map/android.support.test.runner.AndroidJUnitRunner \
| "$REMAPPER" \
"buck-out/gen/$REDEX_ROOT/test/line_map/test_redex/redex-line-number-map"
