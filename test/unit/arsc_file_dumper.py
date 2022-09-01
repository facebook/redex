# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import argparse
import os.path
import subprocess
import tempfile
import zipfile


# Takes an .arsc file, puts it into a .zip file and runs aapt dump command and
# spews out the output.
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--aapt", type=str, required=True)
    parser.add_argument("--arsc", type=str, required=True)
    parser.add_argument("--outfile", type=str, required=True)
    parser.add_argument("--errfile", type=str, required=True)
    args: argparse.Namespace = parser.parse_args()

    with open(args.outfile, "w") as fout:
        with open(args.errfile, "w") as ferr:
            if not os.path.isfile(args.arsc):
                ferr.write("arsc file does not exist: %s" % args.arsc)
                exit(1)

            with tempfile.NamedTemporaryFile() as file:
                with zipfile.ZipFile(file, "w") as temp_zip:
                    temp_zip.write(args.arsc, "resources.arsc")

                cmd = [args.aapt, "d", "--values", "resources", file.name]
                result = subprocess.run(cmd, stdout=fout, stderr=ferr)
                exit(result.returncode)
