#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

tar czf redex.tar.gz redex-all redex.py pyredex/*.py
cat selfextract.sh redex.tar.gz > redex
chmod +x redex
