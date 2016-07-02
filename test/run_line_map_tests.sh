#! /bin/bash

set -e
set -x

buck install native/redex/test/line_map:test_redex
buck build native/redex:line_remap
REMAPPER=`buck targets --show-output native/redex:line_remap | cut -d' ' -f2`

DIR="$(dirname "${BASH_SOURCE[0]}")"
# cd to fbandroid
cd $DIR/../../..

adb shell am instrument -w \
com.facebook.redex.test.line_map/android.support.test.runner.AndroidJUnitRunner \
| "$REMAPPER" \
"buck-out/gen/native/redex/test/line_map/test_redex/redex-line-number-map"
