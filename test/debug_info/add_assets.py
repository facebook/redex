#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import zipfile

from pyredex.utils import add_tool_override, sign_apk


parser = argparse.ArgumentParser()
parser.add_argument("apk", help="Input APK file")
parser.add_argument("assets", help="Files to add to assets/", nargs="+")
parser.add_argument("--keystore")
parser.add_argument("--keypass")
parser.add_argument("--keyalias")
parser.add_argument("--apksigner_path", help="Path to apksigner tool.")
args = parser.parse_args()

with zipfile.ZipFile(args.apk, "a") as zf:
    for asset in args.assets:
        zf.write(asset, os.path.join("assets", os.path.basename(asset)))

add_tool_override("apksigner", args.apksigner_path)
sign_apk(args.keystore, args.keypass, args.keyalias, args.apk)
