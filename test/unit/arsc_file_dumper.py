# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import argparse
import subprocess
import sys
import tempfile
import zipfile


# Takes an .arsc file, puts it into a .zip file and runs aapt dump command and
# spews out the output.
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--aapt", type=str, required=True)
    parser.add_argument("--arsc", type=str, required=True)
    args: argparse.Namespace = parser.parse_args()

    with tempfile.NamedTemporaryFile() as file:
        with zipfile.ZipFile(file, "w") as temp_zip:
            temp_zip.write(args.arsc, "resources.arsc")
        cmd = [args.aapt, "d", "--values", "resources", file.name]
        result = subprocess.run(cmd, stdout=sys.stdout, stderr=sys.stderr)
        exit(result.returncode)
