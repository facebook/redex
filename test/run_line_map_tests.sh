#! /bin/bash
DIR="$(dirname "${BASH_SOURCE[0]}")"
adb shell am instrument -w \
com.facebook.redex.test.line_map/android.support.test.runner.AndroidJUnitRunner \
| "$DIR/line_map/map_lines.py" \
"$DIR/../../../buck-out/gen/native/redex/test/line_map/test_redex_preembed/redex-line-number-map.txt"
