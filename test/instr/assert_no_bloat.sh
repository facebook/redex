#! /bin/bash

# given two APKs, check that the first one's classes.dex is >= the size of the
# second
set -e
set -x

OLD_APK=$1
NEW_APK=$2
[ -f "$OLD_APK" ]
[ -f "$NEW_APK" ]

OLD_SIZE=$(unzip -l "$OLD_APK" classes.dex | tail -n1 | awk '{ print $1 }')
NEW_SIZE=$(unzip -l "$NEW_APK" classes.dex | tail -n1 | awk '{ print $1 }')
[ $OLD_SIZE -ge $NEW_SIZE ]
