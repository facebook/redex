#! /bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

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
