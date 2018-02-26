#!/bin/bash

set -o pipefail

# This script takes either two dexes, two APKs, or two text files which have
# the output of dexdump, and returns a diff of the two with extraneous info
# (like addresses) filtered out .

INPUT=$1
REDEXOUT=$2

IGNORE_RENAME=false
if [[ "$3" == '--ignore-rename' ]]
then
  IGNORE_RENAME=true
fi

TEMPDIR=`mktemp -d 2>/dev/null || mktemp -d -t 'extractdexdump'`
OUTA=$TEMPDIR/alpha.filt.dexdump
OUTB=$TEMPDIR/beta.filt.dexdump
OUTDIFF=$TEMPDIR/diff
if [ ! -f $INPUT ]; then
    echo "No such file $INPUT, bailing"
    exit 1;
fi
DEXDUMP=
if [[ "$INPUT" =~ dex$ ]]; then
    DEXDUMP="dexdump -d"
elif [[ "$INPUT" =~ apk$ ]]; then
    FBANDROID="$(dirname "$(realpath "${BASH_SOURCE[0]}")")/../../.."
    DEXDUMP="$(realpath "$FBANDROID/scripts/ordering/extractdexdump")"
else
    DEXDUMP=cat
fi

export LC_ALL=C

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
    time $GET_DUMP_CMD | \
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

strip_cruft "$DEXDUMP $INPUT" "$OUTA"
strip_cruft "$DEXDUMP $REDEXOUT" "$OUTB"

diff --speed-large-files -u $OUTA $OUTB > $OUTDIFF
if [ $? == 0 ]; then
	echo "The dexes are equivalent"
	exit 0;
else
	echo "The dexes have significant differences"
	echo "The differences are recorded in $OUTDIFF"
	exit 1;
fi
