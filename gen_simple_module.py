#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import base64
import io
import logging
import os
import zipfile
from collections import namedtuple


try:
    import lzma  # noqa(F401)
    import tarfile

    has_tar_lzma = True
except ImportError:
    has_tar_lzma = False


Args = namedtuple("Args", ["inputs", "output", "tarxz"])


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate simple module with the given file"
    )

    parser.add_argument("args", nargs="+", help="name=filename list")
    parser.add_argument(
        "-o",
        "--out",
        nargs=1,
        type=os.path.realpath,
        help="Generated python wrapper",
    )
    parser.add_argument(
        "--force-zip",
        action="store_true",
        help="Force the use of zip, even when tar and lzma are available",
    )

    args = parser.parse_args()

    global has_tar_lzma
    return Args(args.args, args.out[0], has_tar_lzma and not args.force_zip)


def compress_zip(inputs):
    logging.info("Compressing as zip")
    buf = io.BytesIO(b"")
    with zipfile.ZipFile(buf, "w") as zf:
        for input in inputs:
            logging.info("Adding %s", input)
            with open(input, "rb") as f:
                zf.writestr(os.path.basename(input), f)
    buf.seek(0)
    return buf


def compress_tar_xz(inputs):
    logging.info("Compressing as tar.xz")
    buf = io.BytesIO(b"")
    tar = tarfile.open(fileobj=buf, mode="w:xz")

    for input in inputs:
        logging.info("Adding %s", input)
        info = tar.gettarinfo(input)
        info.name = os.path.basename(input)
        with open(input, "rb") as f:
            tar.addfile(info, fileobj=f)

    tar.close()
    buf.seek(0)
    return buf


def compress_and_base_64(inputs, tar_xz):
    with compress_tar_xz(inputs) if tar_xz else compress_zip(inputs) as buf:
        return base64.b64encode(buf.getbuffer())


_FILE_TEMPLATE = """
#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import base64
import io
import re

{extra_imports}


_BASE64BLOB = "{base64_blob}"


{compression_specific_api}


"""


# def get_api_level_file(level):
#     name = f"framework_classes_api_{{level}}.txt"
#     return _load(name)


# def get_api_levels():
#     name_re = re.compile(r"^framework_classes_api_(\\d+)\\.txt$")
#     return {{
#         int(match.group(1)) for name in _all() for match in [name_re.match(name)] if match
#     }}


_TAR_XZ_IMPORTS = """
import lzma  # noqa(F401)
import tarfile
"""
_TAR_XZ_API = """
_TAR = tarfile.open(mode="r:xz", fileobj=io.BytesIO(base64.b64decode(_BASE64BLOB)))


def _load(name):
    global _TAR
    return _TAR.extractfile(name).read()


def _all():
    global _TAR
    return _TAR.getnames()
"""


_ZIP_IMPORTS = "import zipfile"
_ZIP_API = """
_ZIP = zipfile.ZipFile(io.BytesIO(base64.b64decode(_BASE64BLOB)), "r")


def _load(name):
    global _ZIP
    return _ZIP.read(names)


def _all():
    global _ZIP
    return _ZIP.namelist()
"""


def write_py_wrapper(base_64_bytes_blob, files, filename, tar_xz):
    base64_str = base_64_bytes_blob.decode("ascii")
    with open(filename, "w") as f:
        f.write(
            _FILE_TEMPLATE.format(
                extra_imports=_TAR_XZ_IMPORTS if tar_xz else _ZIP_IMPORTS,
                base64_blob=base64_str,
                compression_specific_api=_TAR_XZ_API if tar_xz else _ZIP_API,
            )
        )
        for key, val in list(files.items()):
            f.write(f'{key} = _load("{os.path.basename(val)}")\n')


def main():
    args = parse_args()
    files = {
        key_val[: key_val.find("=")]: key_val[key_val.find("=") + 1 :]
        for key_val in args.inputs
    }
    base64_blob = compress_and_base_64(list(files.values()), args.tarxz)
    write_py_wrapper(base64_blob, files, args.output, args.tarxz)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    main()
