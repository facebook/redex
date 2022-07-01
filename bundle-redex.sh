#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

if [ -f "generated_apilevels.py" ] ; then
  GEN_APILEVELS_INPUT="generated_apilevels.py"
else
  GEN_APILEVELS_INPUT=
fi

tar czf redex.tar.gz redex-all redex.py pyredex/*.py $GEN_APILEVELS_INPUT
cat selfextract.sh redex.tar.gz > redex
chmod +x redex
