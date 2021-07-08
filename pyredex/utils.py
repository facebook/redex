#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import distutils.version
import fnmatch
import glob
import json
import logging
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from os.path import basename, dirname, isfile, join

import pyredex.unpacker
from pyredex.logger import log


IS_WINDOWS = os.name == "nt"
temp_dirs = []


def abs_glob(directory, pattern="*"):
    """
    Returns all files that match the specified glob inside a directory.
    Returns absolute paths. Does not return files that start with '.'
    """
    for result in glob.glob(join(directory, pattern)):
        yield join(directory, result)


def make_temp_dir(name="", debug=False):
    """Make a temporary directory which will be automatically deleted"""
    global temp_dirs
    directory = tempfile.mkdtemp(name)
    if not debug:
        temp_dirs.append(directory)
    return directory


def remove_temp_dirs():
    global temp_dirs
    for directory in temp_dirs:
        shutil.rmtree(directory)


def with_temp_cleanup(fn, always_clean=False):
    success = always_clean
    try:
        fn()
        success = True
    finally:
        if success:
            remove_temp_dirs()


def _find_biggest_build_tools_version(base):
    VERSION_REGEXP = r"\d+\.\d+\.\d+$"
    build_tools = join(base, "build-tools")
    version = max(
        (
            "0.0.1",
            *[d for d in os.listdir(build_tools) if re.match(VERSION_REGEXP, d)],
        ),
        key=distutils.version.StrictVersion,
    )
    if version == "0.0.1":
        return None
    return join(build_tools, version)


def _filter_none_not_exists_ret_none(input):
    if input is None:
        return None
    filtered = [p for p in input if p and os.path.exists(p)]
    if filtered:
        return filtered
    return None


def find_android_path_by_env():
    return _filter_none_not_exists_ret_none(
        [
            os.environ[key]
            for key in ["ANDROID_SDK", "ANDROID_HOME"]
            if key in os.environ
        ]
    )


def find_android_build_tools_by_env():
    base = find_android_path_by_env()
    if not base:
        return None

    return _filter_none_not_exists_ret_none(
        [_find_biggest_build_tools_version(p) for p in base]
    )


# If the script isn't run in a directory that buck recognizes, set this
# to a root dir.
root_dir_for_buck = None


def _load_android_buckconfig_values():
    cmd = ["buck", "audit", "config", "android", "--json"]
    global root_dir_for_buck
    cwd = root_dir_for_buck if root_dir_for_buck is not None else os.getcwd()
    # Set NO_BUCKD to minimize disruption to any currently running buckd
    env = dict(os.environ)
    env["NO_BUCKD"] = "1"
    raw = subprocess.check_output(cmd, cwd=cwd, stderr=subprocess.DEVNULL, env=env)
    return json.loads(raw)


def find_android_path_by_buck():
    logging.debug("Computing SDK path from buck")
    try:
        buckconfig = _load_android_buckconfig_values()
    except BaseException as e:
        logging.debug("Failed loading buckconfig: %s", e)
        return None
    if "android.sdk_path" not in buckconfig:
        return None
    return _filter_none_not_exists_ret_none([buckconfig.get("android.sdk_path")])


def find_android_build_tools_by_buck():
    logging.debug("Computing SDK path from buck")
    try:
        buckconfig = _load_android_buckconfig_values()
    except BaseException as e:
        logging.debug("Failed loading buckconfig: %s", e)
        return None
    if "android.sdk_path" not in buckconfig:
        return None
    sdk_path = buckconfig.get("android.sdk_path")

    if "android.build_tools_version" in buckconfig:
        version = buckconfig["android.build_tools_version"]
        assert isinstance(sdk_path, str)
        return _filter_none_not_exists_ret_none(
            [join(sdk_path, "build-tools", version)]
        )
    else:
        return _filter_none_not_exists_ret_none(
            [_find_biggest_build_tools_version(sdk_path)]
        )


# This order is not necessarily equivalent to buck's. We prefer environment
# variables as they are a lot cheaper.
_sdk_search_order = [
    ("Env", find_android_path_by_env, find_android_build_tools_by_env),
    ("Buck", find_android_path_by_buck, find_android_build_tools_by_buck),
]


def add_android_sdk_path(path):
    global _sdk_search_order
    _sdk_search_order.insert(
        0,
        (
            f"Path:{path}",
            lambda: _filter_none_not_exists_ret_none(
                [
                    # For backwards compatibility
                    *[
                        dirname(dirname(p))
                        for p in [path]
                        if basename(dirname(p)) == "build-tools"
                    ],
                    path,
                ]
            ),
            lambda: _filter_none_not_exists_ret_none(
                [
                    _find_biggest_build_tools_version(path),
                    path,  # For backwards compatibility.
                ]
            ),
        ),
    )


def get_android_sdk_path():
    attempts = []
    global _sdk_search_order
    for name, base_dir_fn, _ in _sdk_search_order:
        logging.debug("Attempting %s to find SDK path", name)
        candidate = base_dir_fn()
        if candidate:
            return candidate[0]
        attempts.append(name)

    raise RuntimeError(f'Could not find SDK path, searched {", ".join(attempts)}')


def find_android_build_tool(tool):
    attempts = []

    def try_find(name, base_dir_fn):
        try:
            if base_dir_fn is None:
                return None
            base_dirs = base_dir_fn()
            if not base_dirs:
                attempts.append(name + ":<Nothing>")
                return None
            for base_dir in base_dirs:
                candidate = join(base_dir, tool)
                if os.path.exists(candidate):
                    return candidate
                attempts.append(name + ":" + base_dir)
        except BaseException:
            pass
        return None

    global _sdk_search_order
    for name, _, base_tools_fn in _sdk_search_order:
        logging.debug("Attempting %s to find %s", name, tool)
        candidate = try_find(name, base_tools_fn)
        if candidate:
            return candidate

    # By `PATH`.
    logging.debug("Attempting PATH to find %s", tool)
    tool_path = shutil.which(tool)
    if tool_path is not None:
        return tool_path
    attempts.append("PATH")

    raise RuntimeError(f'Could not find {tool}, searched {", ".join(attempts)}')


def find_apksigner():
    return find_android_build_tool("apksigner.bat" if IS_WINDOWS else "apksigner")


def remove_signature_files(extracted_apk_dir):
    for f in abs_glob(extracted_apk_dir, "META-INF/*"):
        cert_path = join(extracted_apk_dir, f)
        if isfile(cert_path):
            os.remove(cert_path)


def sign_apk(keystore, keypass, keyalias, apk):
    subprocess.check_call(
        [
            find_apksigner(),
            "sign",
            "--v1-signing-enabled",
            "--v2-signing-enabled",
            "--ks",
            keystore,
            "--ks-pass",
            "pass:" + keypass,
            "--ks-key-alias",
            keyalias,
            apk,
        ],
        stdout=sys.stderr,
    )


def remove_comments_from_line(line):
    (found_backslash, in_quote) = (False, False)
    for idx, c in enumerate(line):
        if c == "\\" and not found_backslash:
            found_backslash = True
        elif c == '"' and not found_backslash:
            found_backslash = False
            in_quote = not in_quote
        elif c == "#" and not in_quote:
            return line[:idx]
        else:
            found_backslash = False
    return line


def remove_comments(lines):
    return "".join([remove_comments_from_line(line) + "\n" for line in lines])


def argparse_yes_no_flag(parser, flag_name, on_prefix="", off_prefix="no-", **kwargs):
    class FlagAction(argparse.Action):
        def __init__(self, option_strings, dest, nargs=None, **kwargs):
            super(FlagAction, self).__init__(option_strings, dest, nargs=0, **kwargs)

        def __call__(self, parser, namespace, values, option_string=None):
            setattr(
                namespace,
                self.dest,
                False if option_string.startswith(f"--{off_prefix}") else True,
            )

    parser.add_argument(
        f"--{on_prefix}{flag_name}",
        f"--{off_prefix}{flag_name}",
        dest=flag_name,
        action=FlagAction,
        default=False,
        **kwargs,
    )


def unzip_apk(apk, destination_directory):
    with zipfile.ZipFile(apk) as z:
        z.extractall(destination_directory)


def extract_dex_number(dexfilename):
    m = re.search(r"(classes|.*-)(\d+)", basename(dexfilename))
    if m is None:
        raise Exception("Bad secondary dex name: " + dexfilename)
    return int(m.group(2))


def dex_glob(directory):
    """
    Return the dexes in a given directory, with the primary dex first.
    """
    primary = join(directory, "classes.dex")
    if not isfile(primary):
        raise Exception("No primary dex found")

    secondaries = [
        d for d in glob.glob(join(directory, "*.dex")) if not d.endswith("classes.dex")
    ]
    secondaries.sort(key=extract_dex_number)

    return [primary] + secondaries


def move_dexen_to_directories(root, dexpaths):
    """
    Move each dex file to its own directory within root and return a list of the
    new paths. Redex will operate on each dex and put the modified dex into the
    same directory.
    """
    res = []
    for idx, dexpath in enumerate(dexpaths):
        dexname = basename(dexpath)
        dirpath = join(root, "dex" + str(idx))
        os.mkdir(dirpath)
        shutil.move(dexpath, dirpath)
        res.append(join(dirpath, dexname))

    return res


def ensure_libs_dir(libs_dir, sub_dir):
    """Ensures the base libs directory and the sub directory exist. Returns top
    most dir that was created.
    """
    if os.path.exists(libs_dir):
        os.mkdir(sub_dir)
        return sub_dir
    else:
        os.mkdir(libs_dir)
        os.mkdir(sub_dir)
        return libs_dir


def get_file_ext(file_name):
    return os.path.splitext(file_name)[1]


class ZipManager:
    """
    __enter__: Unzips input_apk into extracted_apk_dir
    __exit__: Zips extracted_apk_dir into output_apk
    """

    per_file_compression = {}

    def __init__(self, input_apk, extracted_apk_dir, output_apk):
        self.input_apk = input_apk
        self.extracted_apk_dir = extracted_apk_dir
        self.output_apk = output_apk

    def __enter__(self):
        log("Extracting apk...")
        with zipfile.ZipFile(self.input_apk) as z:
            for info in z.infolist():
                self.per_file_compression[info.filename] = info.compress_type
            z.extractall(self.extracted_apk_dir)

    def __exit__(self, *args):
        remove_signature_files(self.extracted_apk_dir)
        if isfile(self.output_apk):
            os.remove(self.output_apk)

        log("Creating output apk")
        with zipfile.ZipFile(self.output_apk, "w") as new_apk:
            # Need sorted output for deterministic zip file. Sorting `dirnames` will
            # ensure the tree walk order. Sorting `filenames` will ensure the files
            # inside the tree.
            # This scheme uses less memory than collecting all files first.
            for dirpath, dirnames, filenames in os.walk(self.extracted_apk_dir):
                dirnames.sort()
                for filename in sorted(filenames):
                    filepath = join(dirpath, filename)
                    archivepath = filepath[len(self.extracted_apk_dir) + 1 :]
                    try:
                        compress = self.per_file_compression[archivepath]
                    except KeyError:
                        compress = zipfile.ZIP_DEFLATED
                    new_apk.write(filepath, archivepath, compress_type=compress)


class UnpackManager:
    """
    __enter__: Unpacks dexes and application modules from extracted_apk_dir into dex_dir
    __exit__: Repacks the dexes and application modules in dex_dir back into extracted_apk_dir
    """

    application_modules = []

    def __init__(
        self,
        input_apk,
        extracted_apk_dir,
        dex_dir,
        have_locators=False,
        debug_mode=False,
        fast_repackage=False,
        reset_timestamps=True,
        is_bundle=False,
    ):
        self.input_apk = input_apk
        self.extracted_apk_dir = extracted_apk_dir
        self.dex_dir = dex_dir
        self.have_locators = have_locators
        self.debug_mode = debug_mode
        self.fast_repackage = fast_repackage
        self.reset_timestamps = reset_timestamps or debug_mode
        self.is_bundle = is_bundle

    def __enter__(self):
        self.dex_mode = pyredex.unpacker.detect_secondary_dex_mode(
            self.extracted_apk_dir, self.is_bundle
        )
        log("Unpacking dex files")
        self.dex_mode.unpackage(self.extracted_apk_dir, self.dex_dir)

        log("Detecting Application Modules")
        store_metadata_dir = make_temp_dir(
            ".application_module_metadata", self.debug_mode
        )
        self.application_modules = pyredex.unpacker.ApplicationModule.detect(
            self.extracted_apk_dir, self.is_bundle
        )
        store_files = []
        for module in self.application_modules:
            canary_prefix = module.get_canary_prefix()
            log(
                "found module: "
                + module.get_name()
                + " "
                + (canary_prefix if canary_prefix is not None else "(no canary prefix)")
            )
            store_path = os.path.join(self.dex_dir, module.get_name())
            os.mkdir(store_path)
            module.unpackage(self.extracted_apk_dir, store_path)
            store_metadata = os.path.join(
                store_metadata_dir, module.get_name() + ".json"
            )
            module.write_redex_metadata(store_path, store_metadata)
            store_files.append(store_metadata)
        return store_files

    def __exit__(self, *args):
        log("Repacking dex files")
        log("Emit Locator Strings: %s" % self.have_locators)

        self.dex_mode.repackage(
            self.extracted_apk_dir,
            self.dex_dir,
            self.have_locators,
            fast_repackage=self.fast_repackage,
            reset_timestamps=self.reset_timestamps,
        )

        locator_store_id = 1
        for module in self.application_modules:
            log(
                "repacking module: "
                + module.get_name()
                + " with id "
                + str(locator_store_id)
            )
            module.repackage(
                self.extracted_apk_dir,
                self.dex_dir,
                self.have_locators,
                locator_store_id,
                fast_repackage=self.fast_repackage,
                reset_timestamps=self.reset_timestamps,
            )
            locator_store_id = locator_store_id + 1


class LibraryManager:
    """
    __enter__: Unpacks additional libraries in extracted_apk_dirs so library class files can be found
    __exit__: Cleanup temp directories used by the class
    """

    temporary_libs_dir = None

    def __init__(self, extracted_apk_dir, is_bundle=False):
        self.extracted_apk_dir = extracted_apk_dir
        self.is_bundle = is_bundle

    def __enter__(self):
        # Some of the native libraries can be concatenated together into one
        # xz-compressed file. We need to decompress that file so that we can scan
        # through it looking for classnames.
        libs_to_extract = []
        xz_lib_name = "libs.xzs"
        zstd_lib_name = "libs.zstd"
        for root, _, filenames in os.walk(self.extracted_apk_dir):
            for filename in fnmatch.filter(filenames, xz_lib_name):
                libs_to_extract.append(join(root, filename))
            for filename in fnmatch.filter(filenames, zstd_lib_name):
                fullpath = join(root, filename)
                # For voltron modules BUCK creates empty zstd files for each module
                if os.path.getsize(fullpath) > 0:
                    libs_to_extract.append(fullpath)
        if len(libs_to_extract) > 0:
            libs_dir = (
                join(self.extracted_apk_dir, "base", "lib")
                if self.is_bundle
                else join(self.extracted_apk_dir, "lib")
            )
            extracted_dir = join(libs_dir, "__extracted_libs__")
            # Ensure all directories exist.
            self.temporary_libs_dir = ensure_libs_dir(libs_dir, extracted_dir)
            for i, lib_to_extract in enumerate(libs_to_extract):
                extract_path = join(extracted_dir, "lib_{}.so".format(i))
                if lib_to_extract.endswith(xz_lib_name):
                    pyredex.unpacker.unpack_xz(lib_to_extract, extract_path)
                else:
                    cmd = 'zstd -d "{}" -o "{}"'.format(lib_to_extract, extract_path)
                    subprocess.check_call(cmd, shell=True)  # noqa: P204

    def __exit__(self, *args):
        # This dir was just here so we could scan it for classnames, but we don't
        # want to pack it back up into the apk
        if self.temporary_libs_dir is not None:
            shutil.rmtree(self.temporary_libs_dir)


# Utility class to reset zip entry timestamps on-the-fly without repackaging.
# Is restricted to single-archive 32-bit zips (like APKs).
class ZipReset:
    @staticmethod
    def reset_array_like(inout, size, date=(1980, 1, 1, 0, 0, 0)):
        eocd_len = 22
        eocd_signature = b"\x50\x4b\x05\x06"
        max_comment_len = 65535
        max_eocd_search = max_comment_len + eocd_len
        lfh_signature = b"\x50\x4b\x03\x04"
        cde_signature = b"\x50\x4b\x01\x02"
        cde_len = 46
        time_code = date[3] << 11 | date[4] << 5 | date[5]
        date_code = (date[0] - 1980) << 9 | date[1] << 5 | date[2]

        def short_le(start):
            return int.from_bytes(inout[start : start + 2], byteorder="little")

        def long_le(start):
            return int.from_bytes(inout[start : start + 4], byteorder="little")

        def put_short_le(start, val):
            inout[start : start + 2] = val.to_bytes(2, byteorder="little")

        def find_eocd_index(len):
            from_index = len - max_eocd_search if len > max_eocd_search else 0
            for i in range(len - 3, from_index - 1, -1):
                if inout[i : i + 4] == eocd_signature:
                    return i
            return None

        def rewrite_entry(index):
            # CDE first.
            assert (
                inout[index : index + 4] == cde_signature
            ), "Did not find CDE signature"
            file_name_len = short_le(index + 28)
            extra_len = short_le(index + 30)
            file_comment_len = short_le(index + 32)
            lfh_index = long_le(index + 42)
            # LFH now.
            assert (
                inout[lfh_index : lfh_index + 4] == lfh_signature
            ), "Did not find LFH signature"

            # Update times.
            put_short_le(index + 12, time_code)
            put_short_le(index + 14, date_code)
            put_short_le(lfh_index + 10, time_code)
            put_short_le(lfh_index + 12, date_code)

            return index + cde_len + file_name_len + extra_len + file_comment_len

        assert size >= eocd_len, "File too small to be a zip"
        eocd_index = find_eocd_index(size)
        assert eocd_index is not None, "Dit not find EOCD"
        assert eocd_index + eocd_len <= size, "EOCD truncated?"

        disk_num = short_le(eocd_index + 4)
        disk_w_cd = short_le(eocd_index + 6)
        num_entries = short_le(eocd_index + 8)
        total_entries = short_le(eocd_index + 10)
        cd_offset = long_le(eocd_index + 16)

        assert (
            disk_num == 0 and disk_w_cd == 0 and num_entries == total_entries
        ), "Archive span unsupported"

        index = cd_offset
        for _i in range(0, total_entries):
            index = rewrite_entry(index)

        assert index == eocd_index, "Failed to reach EOCD again"

    @staticmethod
    def reset_file_into_bytes(filename, date=(1980, 1, 1, 0, 0, 0)):
        with open(filename, "rb") as f:
            data = bytearray(f.read())
        ZipReset.reset_array_like(data, len(data), date)
        return data

    @staticmethod
    def reset_file(filename, date=(1980, 1, 1, 0, 0, 0)):
        with open(filename, "r+b") as f:
            with mmap.mmap(f.fileno(), 0) as map:
                ZipReset.reset_array_like(map, map.size(), date)
                map.flush()
            os.fsync(f.fileno())
