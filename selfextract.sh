#!/bin/bash

export TMPDIR=`mktemp -d /tmp/redex.XXXXXX`
ARCHIVE=`awk '/^__ARCHIVE_BELOW__/ { print NR + 1; exit 0 }' $0`
tail -n+$ARCHIVE $0 | tar xz -C $TMPDIR

$TMPDIR/redex.py $@

rm -rf $TMPDIR
exit 0

__ARCHIVE_BELOW__
