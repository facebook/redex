#! /bin/bash

set -e

TMP=$(mktemp -d -t redex_test_equivalence_XXXXX)
unzip $APK -d $TMP
$GENERATOR $TMP/classes.dex
rm -f $OUT
cd $TMP
rm -f META-INF/*
zip -r $OUT *
jarsigner -sigalg SHA1withRSA -digestalg SHA1 -keystore $KEYSTORE -storepass $KEYPASS $OUT $KEYALIAS
rm -rf $TMP
