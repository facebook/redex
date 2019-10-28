#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -o pipefail

# This script takes either two dexes, two APKs, or two text files which have
# the output of dexdump, and returns a diff of the two with extraneous info
# (like addresses) filtered out .

INPUT_A=$1
INPUT_B=$2

IGNORE_RENAME=false
if [[ "$3" == '--ignore-rename' ]]
then
  IGNORE_RENAME=true
fi

# The script requires dexdump.
command -v dexdump >/dev/null 2>&1 || { echo "Dexdump not found!" ; exit 2; }

TEMPDIR=`mktemp -d 2>/dev/null || mktemp -d -t 'extractdexdump'`

function strip_cruft() {
    local GET_DUMP_CMD="$1"
    local OUT="$2"
    local RENAME_CLASS=''
    local RENAME_CLASS_DEFINITION=''
    if [ $IGNORE_RENAME == true ]
    then
        RENAME_CLASS='s/LX\/...;/LX\/xxx;/g'
        RENAME_CLASS_DEFINITION='s/X\..../X\.xxx/g'
    fi
    echo Running $GET_DUMP_CMD
    $GET_DUMP_CMD | \
        sed 's/^Processing .*dex//' | \
        sed 's/^Opened .*dex//' | \
        sed 's/^  source_file_idx   : [0-9]* /  source_file_idx   : /' | \
        sed 's/^.*|/|/' | \
        sed 's/|\[[0-9a-f]*\]/|\[\]/' | \
        sed 's/^Class #[0-9]*/Class/' | \
        sed 's/type@[0-9a-f]*/type@/' | \
        sed 's/string@[0-9a-f]*/string@/' | \
        sed 's/method@[0-9a-f]*/method@/' | \
        sed 's/field@[0-9a-f]*/field@/' | \
        sed "$RENAME_CLASS" | \
        sed "$RENAME_CLASS_DEFINITION" | \
        sed 's/0x[0-9a-f]* line=[0-9]*//' | \
        sed 's/^|[0-9a-f]*:/|:/' | \
        sed '/^\s*$/d' \
        > "$OUT"
}

# Compare dexfiles in the folder1 and folder2.
function compare_ir_folder() {
    folder1=$1
    folder2=$2
    dex_count1=$(find "$folder1" -name '*.dex'|wc -l)
    dex_count2=$(find "$folder2" -name '*.dex'|wc -l)
    if [ "$dex_count1" -ne "$dex_count2" ]
    then
        echo "[x] Different dex files count, $dex_count1 in $folder1, $dex_count2 in $folder2"
        exit 1
    fi

    for folder in $folder1 $folder2
    do
        for file in irmeta.bin entry.json
        do
            if [ ! -f "$folder/$file" ]
            then
                echo "[x] $folder/$file is missing"
                exit 1
            fi
        done
    done

    # Compare dex files and irmeta.bin. Check md5sum before running dexdump.
    EXIT_STATUS=0
    cd "$folder1" || exit
    md5sum ./*.dex irmeta.bin > "$TEMPDIR/md5.txt"
    cd "$folder2" || exit
    files=$(md5sum -c "$TEMPDIR/md5.txt" --quiet 2>/dev/null | awk -F: '{print $1}')

    cd "$TEMPDIR" || exit
    for f in $files
    do
        if [ "$f" = irmeta.bin ]; then
            echo "[x] irmeta.bin are different"
            EXIT_STATUS=1
            continue
        fi
        strip_cruft "dexdump -d $folder1/$f" "$f.txt.1" >/dev/null
        strip_cruft "dexdump -d $folder2/$f" "$f.txt.2" >/dev/null
        if ! diff --speed-large-files -u "$f.txt.1" "$f.txt.2" > "$f.txt.diff"
        then
            echo "[x] See difference in $TEMPDIR/$f.txt.diff"
            EXIT_STATUS=1
        fi
    done

    test $EXIT_STATUS -eq 0 && echo "[OK] All the dex files and meta data are the same!"
    exit $EXIT_STATUS
}

export LC_ALL=C

if [ -d "$INPUT_A" ]; then
    # Inputs are IR folders, compare the files in the folders and exit.
    compare_ir_folder "$INPUT_A" "$INPUT_B"
    exit 1;
elif [ ! -f "$INPUT_A" ]; then
    echo "No such file $INPUT_A, bailing"
    exit 1;
fi

OUTA=$TEMPDIR/alpha.filt.dexdump
OUTB=$TEMPDIR/beta.filt.dexdump
OUTDIFF=$TEMPDIR/diff
DEXDUMP=
if [[ "$INPUT_A" =~ dex$ ]]; then
    DEXDUMP="dexdump -d"
elif [[ "$INPUT_A" =~ apk$ ]]; then
    FBANDROID="$(dirname "$(realpath "${BASH_SOURCE[0]}")")/../../.."
    DEXDUMP="$(realpath "$FBANDROID/scripts/ordering/extractdexdump")"
else
    DEXDUMP="cat"
fi

strip_cruft "$DEXDUMP $INPUT_A" "$OUTA"
strip_cruft "$DEXDUMP $INPUT_B" "$OUTB"

diff --speed-large-files -u $OUTA $OUTB > $OUTDIFF
if [ $? == 0 ]; then
	echo "The dexes are equivalent"
	exit 0;
else
	echo "The dexes have significant differences"
	echo "The differences are recorded in $OUTDIFF"
	exit 1;
fi
