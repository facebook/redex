# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os

from pyredex.utils import with_temp_cleanup
from tools.bytecode_debugger.inject_debug_lib import run_debug_injector


def arg_parser():
    description = "Injects bytecode-level debug information into an APK"
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=description
    )
    parser.add_argument("bin_path", help="Path to program binary")
    parser.add_argument("input_apk", help="Input APK file")
    parser.add_argument(
        "-o",
        "--output_apk",
        nargs="?",
        type=os.path.realpath,
        default="out.apk",
        help="Output APK file name (defaults to out.apk)",
    )
    parser.add_argument("-s", "--keystore", nargs="?", default=None)
    parser.add_argument("-a", "--keyalias", nargs="?", default=None)
    parser.add_argument("-p", "--keypass", nargs="?", default=None)
    return parser


if __name__ == "__main__":
    args = arg_parser().parse_args()
    with_temp_cleanup(lambda: run_debug_injector(args))
