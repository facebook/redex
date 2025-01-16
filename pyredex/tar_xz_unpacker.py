# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse

import pyredex.unpacker as unpacker


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Utility to unpack a tar gz file on machines that may or may not have gz installed."
    )
    parser.add_argument("--input", required=True, help="Path to tar file")
    parser.add_argument("--out", required=True, help="Path to resulting APK")

    args = parser.parse_args()

    return args


if __name__ == "__main__":
    input_args: argparse.Namespace = parse_args()

    unpacker.unpack_tar_xz(input_args.input, input_args.out)
