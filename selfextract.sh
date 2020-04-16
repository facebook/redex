#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

export TMPDIR=`mktemp -d /tmp/redex.XXXXXX`
ARCHIVE=`awk '/^__ARCHIVE_BELOW__/ { print NR + 1; exit 0 }' $0`
tail -n+$ARCHIVE $0 | tar xz -C $TMPDIR

$TMPDIR/redex.py $@

rm -rf $TMPDIR
exit 0

__ARCHIVE_BELOW__
