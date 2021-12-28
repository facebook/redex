#!/bin/bash -x
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

pwd

PASSES=$(../redex-all --show-passes | grep 'Registered passes' | sed -e 's/.* //')
if [ "$PASSES" -eq 0 ] ; then
  echo "No passes!"
  exit 1
fi
