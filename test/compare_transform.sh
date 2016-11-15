#!/bin/bash

set -o pipefail

# This script takes either two dexes, two APKs, or two text files which have
# the output of dexdump, and returns a diff of the two with extraneous info
# (like addresses) filtered out .

INPUT=$1
REDEXOUT=$2
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
    ROOT=$((git rev-parse --show-toplevel || hg root) 2>/dev/null)
    DEXDUMP="$ROOT/fbandroid/scripts/ordering/extractdexdump"
    if [ ! -f $DEXDUMP ]; then
        DEXDUMP="$ROOT/scripts/ordering/extractdexdump"
    fi
else
    DEXDUMP=cat
fi

export LC_ALL=C

function strip_cruft() {
    local GET_DUMP_CMD="$1"
    local OUT="$2"
    $GET_DUMP_CMD | \
        sed 's/^Processing .*dex//' | \
        sed 's/^Opened .*dex//' | \
        sed 's/^  source_file_idx   : [0-9]* /  source_file_idx   : /' | \
        sed 's/^.*|/|/' | \
        sed 's/|\[[0-9a-f]*\]/|\[\]/' | \
        sed 's/type@[0-9a-f]*/type@/' | \
        sed 's/string@[0-9a-f]*/string@/' | \
        sed 's/method@[0-9a-f]*/method@/' | \
        sed 's/field@[0-9a-f]*/field@/' | \
        sed 's/0x[0-9a-f]* line=[0-9]*//' | \
        sed 's/^|[0-9a-f]*:/|:/' \
        > "$OUT"
}

strip_cruft "$DEXDUMP $INPUT" "$OUTA"
strip_cruft "$DEXDUMP $REDEXOUT" "$OUTB"

diff --speed-large-files -u $OUTA $OUTB | tee $OUTDIFF
if [ $? == 0 ]; then
	echo "The dexes are equivalent"
	exit 0;
else
	echo "FAILURE, the dexes have a significant difference"
	echo "The differences are recorded in $OUTDIFF"
	exit 1;
fi
