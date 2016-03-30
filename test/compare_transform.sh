#!/bin/bash

set -o pipefail

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
    echo "You must specify a dex or apk file as input\"$INPUT\"."
    exit 2;
fi
export LC_ALL=C
$DEXDUMP $INPUT | \
    sed 's/^Processing .*dex//' | \
    sed 's/^Opened .*dex//' | \
    sed 's/^  source_file_idx   : [0-9]* /  source_file_idx   : /' | \
    sed 's/^.*|/|/' | \
    sed 's/|\[[0-9a-f]*\]/|\[\]/' | \
    sed 's/type@[0-9a-f]*/type@/' | \
    sed 's/string@[0-9a-f]*/string@/' | \
    sed 's/method@[0-9a-f]*/method@/' | \
    sed 's/field@[0-9a-f]*/field@/' | \
    sed 's/^|[0-9a-f]*:/|:/' \
    > $OUTA
$DEXDUMP $REDEXOUT | \
    sed 's/^Processing .*dex//' | \
    sed 's/^Opened .*dex//' | \
    sed 's/^  source_file_idx   : [0-9]* /  source_file_idx   : /' | \
    sed 's/^.*|/|/' | \
    sed 's/|\[[0-9a-f]*\]/|\[\]/' | \
    sed 's/type@[0-9a-f]*/type@/' | \
    sed 's/string@[0-9a-f]*/string@/' | \
    sed 's/method@[0-9a-f]*/method@/' | \
    sed 's/field@[0-9a-f]*/field@/' | \
    sed 's/^|[0-9a-f]*:/|:/' \
    > $OUTB

diff --speed-large-files -u $OUTA $OUTB | tee $OUTDIFF
if [ $? == 0 ]; then
	echo "The dexes are equivalent"
	exit 0;
else
	echo "FAILURE, the dexes have a significant difference"
	echo "The differences are recorded in $OUTDIFF"
	exit 1;
fi
