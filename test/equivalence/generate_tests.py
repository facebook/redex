#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import shutil
import subprocess
import tempfile
import zipfile
from os.path import join

from pyredex.utils import sign_apk


parser = argparse.ArgumentParser()
parser.add_argument("apk")
parser.add_argument("--generator", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--keystore", required=True)
parser.add_argument("--keypass", required=True)
parser.add_argument("--keyalias", required=True)
args = parser.parse_args()

with tempfile.TemporaryDirectory() as temp_dir:
    with zipfile.ZipFile(args.apk) as zip:
        zip.extractall(temp_dir)

    subprocess.check_call([args.generator, join(temp_dir, "classes.dex")])

    if os.path.exists(args.output):
        os.remove(args.output)

    shutil.rmtree(join(temp_dir, "META-INF"))
    shutil.make_archive(args.output, "zip", temp_dir)
    # `out`.zip was created, rename.
    os.rename(f"{args.output}.zip", args.output)

    sign_apk(args.keystore, args.keypass, args.keyalias, args.output)
