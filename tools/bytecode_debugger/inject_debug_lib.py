# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import subprocess
import sys

from pyredex.unpacker import LibraryManager, UnpackManager, ZipManager
from pyredex.utils import dex_glob, make_temp_dir, relocate_dexen_to_directories, sign_apk


def run_debug_injector(args):
    extracted_apk_dir = make_temp_dir(".extracted_apk", False)
    dex_dir = make_temp_dir(".dexen", False)

    with ZipManager(args.input_apk, extracted_apk_dir, args.output_apk), UnpackManager(
        args.input_apk, extracted_apk_dir, dex_dir
    ) as store_files, LibraryManager(extracted_apk_dir):
        dexen = relocate_dexen_to_directories(dex_dir, dex_glob(dex_dir)) + store_files
        try:
            subprocess.check_output(
                [args.bin_path, "-o", dex_dir, "--dex-files"] + dexen,
                stderr=subprocess.STDOUT,
            )
        except subprocess.CalledProcessError as e:
            sys.stderr.write("Error while running inject debug binary:\n")
            sys.stderr.write(e.output.decode("utf-8"))
            exit(1)

    if (
        args.keystore is not None
        and args.keyalias is not None
        and args.keypass is not None
    ):
        sign_apk(args.keystore, args.keypass, args.keyalias, args.output_apk)
