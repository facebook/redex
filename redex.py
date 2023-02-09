#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

import argparse
import errno
import glob
import json
import logging
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import timeit
import typing
from os.path import abspath, dirname, getsize, isdir, isfile, join
from pipes import quote

import pyredex.bintools as bintools
import pyredex.logger as logger
from pyredex.buck import BuckConnectionScope, BuckPartScope
from pyredex.packer import compress_entries, CompressionEntry
from pyredex.unpacker import (
    LibraryManager,
    unpack_tar_xz,
    UnpackManager,
    ZipManager,
    ZipReset,
)
from pyredex.utils import (
    add_android_sdk_path,
    argparse_yes_no_flag,
    dex_glob,
    find_android_build_tool,
    get_android_sdk_path,
    get_file_ext,
    make_temp_dir,
    move_dexen_to_directories,
    remove_comments,
    sign_apk,
    with_temp_cleanup,
)


IS_WINDOWS: bool = os.name == "nt"


timer: typing.Callable[[], float] = timeit.default_timer


# Pyre helper.
T = typing.TypeVar("T")


def _assert_val(input: typing.Optional[T]) -> T:
    assert input is not None
    return input


def pgize(name: str) -> str:
    return name.strip()[1:][:-1].replace("/", ".")


def dbg_prefix(dbg: str, src_root: typing.Optional[str] = None) -> typing.List[str]:
    """Return a debugger command prefix.

    `dbg` is either "gdb" or "lldb", indicating which debugger to invoke.
    `src_root` is an optional parameter that indicates the root directory that
        all references to source files in debug information is relative to.

    Returns a list of strings, which when prefixed onto a shell command
        invocation will run that shell command under the debugger.
    """
    assert dbg in ["gdb", "lldb"]

    cmd = [dbg]
    if src_root is not None:
        if dbg == "gdb":
            cmd += ["-ex", quote("directory %s" % src_root)]
        elif dbg == "lldb":
            cmd += ["-o", f"""'settings set target.source-map "." {quote(src_root)}'"""]

            # This makes assumptions about buck-out... I couldn't find an easy
            # way to just get the config file beside this script...
            dir_name = dirname(abspath(__file__))
            dir_name = dirname(dir_name)
            lldbinit_file = dir_name + "/.lldbinit/.lldbinit"
            if isfile(lldbinit_file):
                cmd += ["-o", f"'command source {quote(lldbinit_file)}'"]

    DBG_END = {"gdb": "--args", "lldb": "--"}
    cmd.append(DBG_END[dbg])

    return cmd


def write_debugger_command(
    dbg: str, src_root: typing.Optional[str], args: typing.Iterable[str]
) -> str:
    """Write out a shell script that allows us to rerun redex-all under a debugger.

    The choice of debugger is governed by `dbg` which can be either "gdb" or "lldb".
    """
    assert not IS_WINDOWS  # It's a Linux/Mac script...
    fd, script_name = tempfile.mkstemp(suffix=".sh", prefix="redex-{}-".format(dbg))

    # Parametrise redex binary.
    args = [quote(a) for a in args]
    redex_binary = args[0]
    args[0] = '"$REDEX_BINARY"'

    with os.fdopen(fd, "w") as f:
        f.write("#! /usr/bin/env bash\n")
        f.write('REDEX_BINARY="${REDEX_BINARY:-%s}"\n' % redex_binary)
        f.write("cd %s || exit\n" % quote(os.getcwd()))
        f.write(" ".join(dbg_prefix(dbg, src_root)))
        f.write(" ")
        f.write(" ".join(args))
        if not IS_WINDOWS:
            os.fchmod(fd, 0o775)  # This is unsupported on windows.

    return script_name


def add_extra_environment_args(env: typing.Dict[str, str]) -> None:
    # If we haven't set MALLOC_CONF but we have requested to profile the memory
    # of a specific pass, set some reasonable defaults
    if "MALLOC_PROFILE_PASS" in env and "MALLOC_CONF" not in env:
        env[
            "MALLOC_CONF"
        ] = "prof:true,prof_prefix:jeprof.out,prof_gdump:true,prof_active:false"

    # If we haven't set MALLOC_CONF, tune MALLOC_CONF for better perf
    if "MALLOC_CONF" not in env:
        env["MALLOC_CONF"] = "background_thread:true,metadata_thp:always,thp:always"


def get_stop_pass_idx(passes_list: typing.Iterable[str], pass_name_and_num: str) -> int:
    # Get the stop position
    # pass_name_and num may be "MyPass#0", "MyPass#3" or "MyPass"
    pass_name = pass_name_and_num
    pass_order = 0
    if "#" in pass_name_and_num:
        pass_name, pass_order = pass_name_and_num.split("#", 1)
        try:
            pass_order = int(pass_order)
        except ValueError:
            sys.exit(
                "Invalid stop-pass %s, should be in 'SomePass(#num)'"
                % pass_name_and_num
            )
    cur_order = 0
    for _idx, _name in enumerate(passes_list):
        if _name == pass_name:
            if cur_order == pass_order:
                return _idx
            else:
                cur_order += 1
    sys.exit(
        "Invalid stop-pass %s. %d %s in passes_list"
        % (pass_name_and_num, cur_order, pass_name)
    )


class ExceptionMessageFormatter:
    def format_rerun_message(self, gdb_script_name: str, lldb_script_name: str) -> str:
        return "You can re-run it under gdb by running {} or under lldb by running {}".format(
            gdb_script_name, lldb_script_name
        )

    def format_message(
        self,
        err_out: typing.List[str],
        default_error_msg: str,
        gdb_script_name: str,
        lldb_script_name: str,
    ) -> str:
        return "{} {}".format(
            default_error_msg,
            self.format_rerun_message(gdb_script_name, lldb_script_name),
        )


class State(object):
    # This structure is only used for passing arguments between prepare_redex,
    # launch_redex_binary, finalize_redex
    def __init__(
        self,
        args: argparse.Namespace,
        config_dict: typing.Dict[str, typing.Any],
        debugger: typing.Optional[str],
        dex_dir: str,
        dexen: typing.List[str],
        extracted_apk_dir: typing.Optional[str],
        stop_pass_idx: int,
        lib_manager: typing.Optional[LibraryManager],
        unpack_manager: typing.Optional[UnpackManager],
        zip_manager: typing.Optional[ZipManager],
    ) -> None:
        self.args = args
        self.config_dict = config_dict
        self.debugger = debugger
        self.dex_dir = dex_dir
        self.dexen = dexen
        self.extracted_apk_dir = extracted_apk_dir
        self.stop_pass_idx = stop_pass_idx
        self.lib_manager = lib_manager
        self.unpack_manager = unpack_manager
        self.zip_manager = zip_manager


class RedexRunException(Exception):
    def __init__(
        self,
        msg: str,
        return_code: int,
        abort_error: typing.Optional[str],
        symbolized: typing.Optional[typing.List[str]],
    ) -> None:
        super().__init__(msg, return_code, abort_error, symbolized)
        self.msg = msg
        self.return_code = return_code
        self.abort_error = abort_error
        self.symbolized = symbolized

    def __str__(self) -> str:
        return self.msg


def run_redex_binary(
    state: State,
    exception_formatter: ExceptionMessageFormatter,
    output_line_handler: typing.Optional[typing.Callable[[str], str]],
) -> None:
    if state.args.redex_binary is None:
        state.args.redex_binary = shutil.which("redex-all")

    if state.args.redex_binary is None:
        # __file__ can be /path/fb-redex.pex/redex.pyc
        dir_name = dirname(abspath(__file__))
        while not isdir(dir_name):
            dir_name = dirname(dir_name)
        state.args.redex_binary = join(dir_name, "redex-all")
    if not isfile(state.args.redex_binary) or not os.access(
        state.args.redex_binary, os.X_OK
    ):
        sys.exit(
            "redex-all is not found or is not executable: " + state.args.redex_binary
        )
    logging.debug("Running redex binary at %s", state.args.redex_binary)

    args: typing.List[str] = [state.args.redex_binary]

    args += (
        (["--dex-files"] + state.args.dex_files)
        if state.args.dex_files
        else ["--apkdir", _assert_val(state.extracted_apk_dir)]
    )
    args += ["--outdir", state.dex_dir]

    if state.args.cmd_prefix is not None:
        args = shlex.split(state.args.cmd_prefix) + args

    if state.args.config:
        args += ["--config", state.args.config]

    if state.args.verify_none_mode or state.config_dict.get("verify_none_mode"):
        args += ["--verify-none-mode"]

    if state.args.is_art_build:
        args += ["--is-art-build"]

    if state.args.redacted:
        args += ["--redacted"]

    if state.args.disable_dex_hasher:
        args += ["--disable-dex-hasher"]

    if state.args.enable_instrument_pass or state.config_dict.get(
        "enable_instrument_pass"
    ):
        args += ["--enable-instrument-pass"]

    if state.args.warn:
        args += ["--warn", state.args.warn]
    args += ["--proguard-config=" + x for x in state.args.proguard_configs]
    if state.args.proguard_map:
        args += ["-Sproguard_map=" + state.args.proguard_map]

    args += ["--jarpath=" + x for x in state.args.jarpaths]
    if state.args.printseeds:
        args += ["--printseeds=" + state.args.printseeds]
    if state.args.used_js_assets:
        args += ["--used-js-assets=" + x for x in state.args.used_js_assets]
    if state.args.arch:
        args += ["--arch=" + state.args.arch]
    if state.args.jni_summary:
        args += ["--jni-summary=" + state.args.jni_summary]
    args += ["-S" + x for x in state.args.passthru]
    args += ["-J" + x for x in state.args.passthru_json]

    args += state.dexen

    # Stop before a pass and output intermediate dex and IR meta data.
    if state.stop_pass_idx != -1:
        args += [
            "--stop-pass",
            str(state.stop_pass_idx),
            "--output-ir",
            state.args.output_ir,
        ]

    debugger = state.debugger
    prefix: typing.List[str] = (
        dbg_prefix(debugger, state.args.debug_source_root)
        if debugger is not None
        else []
    )
    start = timer()

    if state.args.debug:
        print("cd %s && %s" % (os.getcwd(), " ".join(prefix + list(map(quote, args)))))
        sys.exit()

    env: typing.Dict[str, str] = os.environ.copy()
    if state.args.quiet:
        # Remove TRACE if it exists.
        env.pop("TRACE", None)
    else:
        # Check whether TRACE is set. If not, use "TIME:1,PM:1".
        if "TRACE" not in env:
            env["TRACE"] = "TIME:1,PM:1"

    env = logger.setup_trace_for_child(env)
    logger.flush()

    add_extra_environment_args(env)

    def run() -> None:
        with bintools.SigIntHandler() as sigint_handler:
            trace_fp = logger.get_trace_file()
            pass_fds = [trace_fp.fileno()] if trace_fp is not sys.stderr else []

            proc, handler = bintools.run_and_stream_stderr(prefix + args, env, pass_fds)
            sigint_handler.set_started(proc)

            returncode, err_out = handler(output_line_handler)

            sigint_handler.set_postprocessing()

            if returncode == 0:
                return

            # Check for crash traces.
            symbolized = bintools.maybe_addr2line(err_out)
            if symbolized:
                sys.stderr.write("\n")
                sys.stderr.write("\n".join(symbolized))
                sys.stderr.write("\n")

            abort_error = None
            if returncode == -6:  # SIGABRT
                abort_error = bintools.find_abort_error(err_out)

            default_error_msg = "redex-all crashed with exit code {}!{}".format(
                returncode, "\n" + abort_error if abort_error else ""
            )
            if IS_WINDOWS:
                raise RuntimeError(default_error_msg)

            gdb_script_name = write_debugger_command(
                "gdb", state.args.debug_source_root, args
            )
            lldb_script_name = write_debugger_command(
                "lldb", state.args.debug_source_root, args
            )
            msg = exception_formatter.format_message(
                err_out,
                default_error_msg,
                gdb_script_name,
                lldb_script_name,
            )
            raise RedexRunException(msg, returncode, abort_error, symbolized)

    # Our CI system occasionally fails because it is trying to write the
    # redex-all binary when this tries to run.  This shouldn't happen, and
    # might be caused by a JVM bug.  Anyways, let's retry and hope it stops.
    for i in range(5):
        try:
            run()
            break
        except OSError as err:
            if err.errno == errno.ETXTBSY and i < 4:
                continue
            raise err

    logging.debug("Dex processing finished in {:.2f} seconds".format(timer() - start))


def zipalign(
    unaligned_apk_path: str,
    output_apk_path: str,
    ignore_zipalign: bool,
    page_align: bool,
) -> None:
    # Align zip and optionally perform good compression.
    try:
        zipalign = [
            find_android_build_tool("zipalign.exe" if IS_WINDOWS else "zipalign"),
            "4",
            unaligned_apk_path,
            output_apk_path,
        ]
        if page_align:
            zipalign.insert(1, "-p")

        p = subprocess.Popen(zipalign, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out, _ = p.communicate()
        if p.returncode == 0:
            os.remove(unaligned_apk_path)
            return
        out_str = out.decode(sys.getfilesystemencoding())
        raise RuntimeError("Failed to execute zipalign, output: {}".format(out_str))
    except OSError as e:
        if e.errno == errno.ENOENT:
            print("Couldn't find zipalign. See README.md to resolve this.")
        if not ignore_zipalign:
            raise e
        shutil.copy(unaligned_apk_path, output_apk_path)
    except BaseException:
        if not ignore_zipalign:
            raise
        shutil.copy(unaligned_apk_path, output_apk_path)
    os.remove(unaligned_apk_path)


def align_and_sign_output_apk(
    unaligned_apk_path: str,
    output_apk_path: str,
    reset_timestamps: bool,
    sign: bool,
    keystore: str,
    key_alias: str,
    key_password: str,
    ignore_zipalign: bool,
    page_align: bool,
) -> None:
    if isfile(output_apk_path):
        os.remove(output_apk_path)

    try:
        os.makedirs(dirname(output_apk_path))
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise

    zipalign(unaligned_apk_path, output_apk_path, ignore_zipalign, page_align)

    if reset_timestamps:
        ZipReset.reset_file(output_apk_path)

    # Add new signature
    if sign:
        sign_apk(keystore, key_password, key_alias, output_apk_path)


def copy_file_to_out_dir(
    tmp: str, apk_output_path: str, name: str, human_name: str, out_name: str
) -> None:
    output_dir = os.path.dirname(apk_output_path)
    output_path = os.path.join(output_dir, out_name)
    tmp_path = tmp + "/" + name
    if os.path.isfile(tmp_path):
        shutil.copy2(tmp_path, output_path)
        logging.debug("Copying " + human_name + " map to output_dir: " + output_path)
    else:
        logging.debug("Skipping " + human_name + " copy, since no file found to copy")


def copy_all_file_to_out_dir(
    tmp: str, apk_output_path: str, ext: str, human_name: str
) -> None:
    tmp_path = tmp + "/" + ext
    for file in glob.glob(tmp_path):
        filename = os.path.basename(file)
        copy_file_to_out_dir(
            tmp, apk_output_path, filename, human_name + " " + filename, filename
        )


def validate_args(args: argparse.Namespace) -> None:
    if args.sign:
        for arg_name in ["keystore", "keyalias", "keypass"]:
            if getattr(args, arg_name) is None:
                raise argparse.ArgumentTypeError(
                    "Could not find a suitable default for --{} and no value "
                    "was provided.  This argument is required when --sign "
                    "is used".format(arg_name)
                )


def arg_parser(
    binary: typing.Optional[str] = None,
    config: typing.Optional[str] = None,
    keystore: typing.Optional[str] = None,
    keyalias: typing.Optional[str] = None,
    keypass: typing.Optional[str] = None,
) -> argparse.ArgumentParser:
    description = """
Given an APK, produce a better APK!

"""
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=description
    )

    parser.add_argument("input_apk", nargs="?", help="Input APK file")
    parser.add_argument(
        "-o",
        "--out",
        nargs="?",
        type=os.path.realpath,
        default="redex-out.apk",
        help="Output APK file name (defaults to redex-out.apk)",
    )
    parser.add_argument(
        "-j",
        "--jarpath",
        dest="jarpaths",
        action="append",
        default=[],
        help="Path to dependent library jar file",
    )

    parser.add_argument(
        "--redex-binary", nargs="?", default=binary, help="Path to redex binary"
    )

    parser.add_argument("-c", "--config", default=config, help="Configuration file")

    argparse_yes_no_flag(parser, "sign", help="Sign the apk after optimizing it")
    parser.add_argument("-s", "--keystore", nargs="?", default=keystore)
    parser.add_argument("-a", "--keyalias", nargs="?", default=keyalias)
    parser.add_argument("-p", "--keypass", nargs="?", default=keypass)

    parser.add_argument(
        "-u",
        "--unpack-only",
        action="store_true",
        help="Unpack the apk and print the unpacked directories, don't "
        "run any redex passes or repack the apk",
    )

    parser.add_argument(
        "--unpack-dest",
        nargs=1,
        help="Specify the base name of the destination directories; works with -u",
    )

    parser.add_argument("-w", "--warn", nargs="?", help="Control verbosity of warnings")

    parser.add_argument(
        "-d",
        "--debug",
        action="store_true",
        help="Unpack the apk and print the redex command line to run",
    )

    parser.add_argument(
        "--dev", action="store_true", help="Optimize for development speed"
    )

    parser.add_argument(
        "-m",
        "--proguard-map",
        nargs="?",
        help="Path to proguard mapping.txt for deobfuscating names",
    )

    parser.add_argument("--printseeds", nargs="?", help="File to print seeds to")

    parser.add_argument(
        "--used-js-assets",
        action="append",
        default=[],
        help="A JSON file (or files) containing a list of resources used by JS",
    )

    parser.add_argument(
        "-P",
        "--proguard-config",
        dest="proguard_configs",
        action="append",
        default=[],
        help="Path to proguard config",
    )

    parser.add_argument(
        "-k",
        "--keep",
        nargs="?",
        help="[deprecated] Path to file containing classes to keep",
    )

    parser.add_argument(
        "-A",
        "--arch",
        nargs="?",
        help='Architecture; one of arm/armv7/arm64/x86_64/x86"',
    )

    parser.add_argument(
        "-S",
        dest="passthru",
        action="append",
        default=[],
        help="Arguments passed through to redex",
    )
    parser.add_argument(
        "-J",
        dest="passthru_json",
        action="append",
        default=[],
        help="JSON-formatted arguments passed through to redex",
    )

    parser.add_argument("--lldb", action="store_true", help="Run redex binary in lldb")
    parser.add_argument("--gdb", action="store_true", help="Run redex binary in gdb")
    parser.add_argument(
        "--ignore-zipalign", action="store_true", help="Ignore if zipalign is not found"
    )
    parser.add_argument(
        "--verify-none-mode",
        action="store_true",
        help="Enable verify-none mode on redex",
    )
    parser.add_argument(
        "--enable-instrument-pass",
        action="store_true",
        help="Enable InstrumentPass if any",
    )
    parser.add_argument(
        "--is-art-build",
        action="store_true",
        help="States that this is an art only build",
    )
    parser.add_argument(
        "--redacted",
        action="store_true",
        default=False,
        help="Specifies how dex files should be laid out",
    )
    parser.add_argument(
        "--disable-dex-hasher", action="store_true", help="Disable DexHasher"
    )
    parser.add_argument(
        "--page-align-libs",
        action="store_true",
        help="Preserve 4k page alignment for uncompressed libs",
    )

    parser.add_argument(
        "--side-effect-summaries", help="Side effect information for external methods"
    )

    parser.add_argument(
        "--escape-summaries", help="Escape information for external methods"
    )

    parser.add_argument(
        "--stop-pass",
        default="",
        help="Stop before a pass and dump intermediate dex and IR meta data to a directory",
    )
    parser.add_argument(
        "--output-ir",
        default="",
        help="Stop before stop_pass and dump intermediate dex and IR meta data to output_ir folder",
    )
    parser.add_argument(
        "--debug-source-root",
        default=None,
        nargs="?",
        help="Root directory that all references to source files in debug information is given relative to.",
    )

    parser.add_argument(
        "--always-clean-up",
        action="store_true",
        help="Clean up temporaries even under failure",
    )

    parser.add_argument("--cmd-prefix", type=str, help="Prefix redex-all with")

    parser.add_argument(
        "--reset-zip-timestamps",
        action="store_true",
        help="Reset zip timestamps for deterministic output",
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Do not be verbose, and override TRACE.",
    )

    parser.add_argument("--android-sdk-path", type=str, help="Path to Android SDK")

    parser.add_argument(
        "--suppress-android-jar-check",
        action="store_true",
        help="Do not look for an `android.jar` in the jar paths",
    )

    parser.add_argument(
        "--log-level",
        default="warning",
        help="Specify the python logging level",
    )

    parser.add_argument(
        "--packed-profiles",
        type=str,
        help="Path to packed profiles (expects tar.xz)",
    )

    parser.add_argument(
        "--secondary-packed-profiles",
        type=str,
        help="Path to packed secondary profiles (expects tar.xz)",
    )

    parser.add_argument(
        "--jni-summary",
        default=None,
        type=str,
        help="Path to JNI summary directory of json files.",
    )

    # Passthrough mode.
    parser.add_argument("--outdir", type=str)
    parser.add_argument("--dex-files", nargs="+", default=[])

    return parser


def _has_android_library_jars(pg_file: str) -> bool:
    # We do not tokenize properly here. Minimum effort.
    def _gen() -> typing.Generator[str, None, None]:
        with open(pg_file, "r") as f:
            for line in f:
                yield line.strip()

    gen = _gen()
    for line in gen:
        if line == "-libraryjars":
            line = next(gen, "a")
            parts = line.split(":")
            for p in parts:
                if p.endswith("android.jar"):
                    return True
    return False


def _check_android_sdk(args: argparse.Namespace) -> None:
    if args.suppress_android_jar_check:
        logging.debug("No SDK jar check done")
        return

    for jarpath in args.jarpaths:
        if jarpath.endswith("android.jar"):
            logging.debug("Found an SDK-looking jar: %s", jarpath)
            return

    for pg_config in args.proguard_configs:
        if _has_android_library_jars(pg_config):
            logging.debug("Found an SDK-looking jar in PG file %s", pg_config)
            return

    # Check whether we can find and add one.
    logging.info(
        "No SDK jar found, attempting to find one. If the detection is wrong, add `--suppress-android-jar-check`."
    )

    try:
        sdk_path = get_android_sdk_path()
        logging.debug("SDK path is %s", sdk_path)
        platforms = join(sdk_path, "platforms")
        if not os.path.exists(platforms):
            raise RuntimeError("platforms directory does not exist")
        VERSION_REGEXP = r"android-(\d+)"
        version = max(
            (
                -1,
                *[
                    int(m.group(1))
                    for d in os.listdir(platforms)
                    for m in [re.match(VERSION_REGEXP, d)]
                    if m
                ],
            ),
        )
        if version == -1:
            raise RuntimeError(f"No android jar directories found in {platforms}")
        jar_path = join(platforms, f"android-{version}", "android.jar")
        if not os.path.exists(jar_path):
            raise RuntimeError(f"{jar_path} not found")
        logging.info("Adding SDK jar path %s", jar_path)
        args.jarpaths.append(jar_path)
    except BaseException as e:
        logging.warning("Could not find an SDK jar: %s", e)


def _has_config_val(args: argparse.Namespace, path: typing.Iterable[str]) -> bool:
    try:
        with open(args.config, "r") as f:
            json_obj = json.load(f)
        for item in path:
            if item not in json_obj:
                logging.debug("Did not find %s in %s", item, json_obj)
                return False
            json_obj = json_obj[item]
        return True
    except BaseException as e:
        logging.error("%s", e)
        return False


def _check_shrinker_heuristics(args: argparse.Namespace) -> None:
    arg_template = "inliner.reg_alloc_random_forest="
    for arg in args.passthru:
        if arg.startswith(arg_template):
            return

    if _has_config_val(args, ["inliner", "reg_alloc_random_forest"]):
        return

    # Nothing found, check whether we have files embedded
    logging.info("No shrinking heuristic found, searching for default.")
    try:
        # pyre-ignore[21]
        from generated_shrinker_regalloc_heuristics import SHRINKER_HEURISTICS_FILE

        logging.info("Found embedded shrinker heuristics")
        tmp_dir = make_temp_dir("shrinker_heuristics")
        filename = os.path.join(tmp_dir, "shrinker.forest")
        logging.info("Writing shrinker heuristics to %s", filename)
        with open(filename, "wb") as f:
            f.write(SHRINKER_HEURISTICS_FILE)
        arg = arg_template + filename
        args.passthru.append(arg)
    except ImportError:
        logging.info("No embedded files, please add manually!")


def _check_android_sdk_api(args: argparse.Namespace) -> None:
    arg_template = "android_sdk_api_{level}_file="
    arg_re = re.compile("^" + arg_template.format(level="(\\d+)"))
    for arg in args.passthru:
        if arg_re.match(arg):
            return

    # Nothing found, check whether we have files embedded
    logging.info("No android_sdk_api_XX_file parameters found.")
    try:
        # pyre-ignore[21]
        import generated_apilevels as ga

        levels = ga.get_api_levels()
        logging.info("Found embedded API levels: %s", levels)
        api_dir = make_temp_dir("api_levels")
        logging.info("Writing API level files to %s", api_dir)
        for level in levels:
            blob = ga.get_api_level_file(level)
            filename = os.path.join(api_dir, f"framework_classes_api_{level}.txt")
            with open(filename, "wb") as f:
                f.write(blob)
            arg = arg_template.format(level=level) + filename
            args.passthru.append(arg)
    except ImportError:
        logging.warning("No embedded files, please add manually!")


def _handle_profiles(args: argparse.Namespace) -> None:
    if not args.packed_profiles:
        return

    directory = make_temp_dir(".redex_profiles", False)
    unpack_tar_xz(args.packed_profiles, directory)

    # Create input for method profiles.
    method_profiles_str = ", ".join(
        f'"{f.path}"'
        for f in os.scandir(directory)
        if f.is_file() and ("method_stats" in f.name or "agg_stats" in f.name)
    )
    if method_profiles_str:
        logging.debug("Found method profiles: %s", method_profiles_str)
        args.passthru_json.append(f"agg_method_stats_files=[{method_profiles_str}]")
    else:
        logging.info("No method profiles found in %s", args.packed_profiles)

    # Create input for basic blocks.
    # Note: at the moment, only look for ColdStart.
    join_str = ";" if IS_WINDOWS else ":"
    block_profiles_str = join_str.join(
        f"{f.path}"
        for f in os.scandir(directory)
        if f.is_file() and f.name.startswith("block_profiles_")
    )
    if block_profiles_str:
        logging.debug("Found block profiles: %s", block_profiles_str)
        # Assume there's at most one.
        args.passthru.append(
            f"InsertSourceBlocksPass.profile_files={block_profiles_str}"
        )
    else:
        logging.info("No block profiles found in %s", args.packed_profiles)


def _handle_secondary_method_profiles(args: argparse.Namespace) -> None:
    if not args.secondary_packed_profiles:
        return

    directory = make_temp_dir(".redex_profiles", False)
    unpack_tar_xz(args.secondary_packed_profiles, directory)

    # Create input for secondary method profiles.
    secondary_method_profiles_str = ", ".join(
        f'"{f.path}"'
        for f in os.scandir(directory)
        if f.is_file()
        and ("secondary_method_stats" in f.name or "secondary_stats" in f.name)
    )
    if secondary_method_profiles_str:
        logging.debug(
            "Found secondary_method profiles: %s", secondary_method_profiles_str
        )
        args.passthru_json.append(
            f"secondary_method_stats_files=[{secondary_method_profiles_str}]"
        )
    else:
        logging.info(
            "No secondary method profiles found in %s", args.secondary_packed_profiles
        )


def prepare_redex(args: argparse.Namespace) -> State:
    logging.debug("Preparing...")
    debug_mode = args.unpack_only or args.debug

    if args.android_sdk_path:
        add_android_sdk_path(args.android_sdk_path)

    # avoid accidentally mixing up file formats since we now support
    # both apk files and Android bundle files
    file_ext = get_file_ext(args.input_apk)
    if not args.unpack_only:
        assert file_ext == get_file_ext(args.out), (
            'Input file extension ("'
            + file_ext
            + '") should be the same as output file extension ("'
            + get_file_ext(args.out)
            + '")'
        )

    extracted_apk_dir = None
    dex_dir = None
    if args.unpack_only and args.unpack_dest:
        if args.unpack_dest[0] == ".":
            # Use APK's name
            unpack_dir_basename = os.path.splitext(args.input_apk)[0]
        else:
            unpack_dir_basename = args.unpack_dest[0]
        extracted_apk_dir = unpack_dir_basename + ".redex_extracted_apk"
        dex_dir = unpack_dir_basename + ".redex_dexen"
        try:
            os.makedirs(extracted_apk_dir)
            os.makedirs(dex_dir)
            extracted_apk_dir = os.path.abspath(extracted_apk_dir)
            dex_dir = os.path.abspath(dex_dir)
        except OSError as e:
            if e.errno == errno.EEXIST:
                print("Error: destination directory already exists!")
                print("APK: " + extracted_apk_dir)
                print("DEX: " + dex_dir)
                sys.exit(1)
            raise e

    config = args.config
    binary = args.redex_binary
    logging.debug("Using config %s", config if config is not None else "(default)")
    logging.debug("Using binary %s", binary if binary is not None else "(default)")

    if args.unpack_only or config is None:
        config_dict = {}
    else:
        with open(config) as config_file:
            try:
                lines = config_file.readlines()
                config_dict = json.loads(remove_comments(lines))
            except ValueError:
                raise ValueError(
                    "Invalid JSON in ReDex config file: %s" % config_file.name
                )

    # stop_pass_idx >= 0 means need stop before a pass and dump intermediate result
    stop_pass_idx = -1
    if args.stop_pass:
        passes_list = config_dict.get("redex", {}).get("passes", [])
        stop_pass_idx = get_stop_pass_idx(passes_list, args.stop_pass)
        if not args.output_ir or isfile(args.output_ir):
            print("Error: output_ir should be a directory")
            sys.exit(1)
        try:
            os.makedirs(args.output_ir)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise e

    with BuckPartScope("redex::Unpacking", "Unpacking Redex input"):
        logging.debug("Unpacking...")
        unpack_start_time = timer()
        if not extracted_apk_dir:
            extracted_apk_dir = make_temp_dir(".redex_extracted_apk", debug_mode)

        directory = make_temp_dir(".redex_unaligned", False)
        unaligned_apk_path = join(directory, "redex-unaligned." + file_ext)
        zip_manager = ZipManager(args.input_apk, extracted_apk_dir, unaligned_apk_path)
        zip_manager.__enter__()

        if not dex_dir:
            dex_dir = make_temp_dir(".redex_dexen", debug_mode)

        is_bundle = isfile(join(extracted_apk_dir, "BundleConfig.pb"))
        unpack_manager = UnpackManager(
            args.input_apk,
            extracted_apk_dir,
            dex_dir,
            have_locators=config_dict.get("emit_locator_strings"),
            debug_mode=debug_mode,
            fast_repackage=args.dev,
            reset_timestamps=args.reset_zip_timestamps or args.dev,
            is_bundle=is_bundle,
        )
        store_files = unpack_manager.__enter__()

        lib_manager = LibraryManager(extracted_apk_dir, is_bundle=is_bundle)
        lib_manager.__enter__()

        if args.unpack_only:
            print("APK: " + extracted_apk_dir)
            print("DEX: " + dex_dir)
            sys.exit()

    # Unpack profiles, if they exist.
    _handle_profiles(args)
    _handle_secondary_method_profiles(args)

    logging.debug("Moving contents to expected structure...")
    # Move each dex to a separate temporary directory to be operated by
    # redex.
    dexen = move_dexen_to_directories(dex_dir, dex_glob(dex_dir))
    for store in sorted(store_files):
        dexen.append(store)
    logging.debug(
        "Unpacking APK finished in {:.2f} seconds".format(timer() - unpack_start_time)
    )

    if args.side_effect_summaries is not None:
        args.passthru_json.append(
            'ObjectSensitiveDcePass.side_effect_summaries="%s"'
            % args.side_effect_summaries
        )

    if args.escape_summaries is not None:
        args.passthru_json.append(
            'ObjectSensitiveDcePass.escape_summaries="%s"' % args.escape_summaries
        )

    for key_value_str in args.passthru_json:
        key_value = key_value_str.split("=", 1)
        if len(key_value) != 2:
            logging.debug(
                "Json Pass through %s is not valid. Split len: %s",
                key_value_str,
                len(key_value),
            )
            continue
        key = key_value[0]
        value = key_value[1]
        prev_value = config_dict.get(key, "(No previous value)")
        logging.debug(
            "Got Override %s = %s from %s. Previous %s",
            key,
            value,
            key_value_str,
            prev_value,
        )
        config_dict[key] = json.loads(value)

    # Scan for framework files. If not found, warn and add them if available.
    _check_android_sdk_api(args)
    # Check for shrinker heuristics.
    _check_shrinker_heuristics(args)

    # Scan for SDK jar. If not found, warn and add if available.
    _check_android_sdk(args)

    logging.debug("Running redex-all on %d dex files ", len(dexen))
    if args.lldb:
        debugger = "lldb"
    elif args.gdb:
        debugger = "gdb"
    else:
        debugger = None

    return State(
        args=args,
        config_dict=config_dict,
        debugger=debugger,
        dex_dir=dex_dir,
        dexen=dexen,
        extracted_apk_dir=extracted_apk_dir,
        stop_pass_idx=stop_pass_idx,
        lib_manager=lib_manager,
        unpack_manager=unpack_manager,
        zip_manager=zip_manager,
    )


def get_compression_list() -> typing.List[CompressionEntry]:
    return [
        CompressionEntry(
            "Redex Instrumentation Metadata",
            lambda args: args.enable_instrument_pass,
            True,
            ["redex-instrument-metadata.txt"],
            [
                "redex-source-block-method-dictionary.csv",
                "redex-source-blocks.csv",
                "redex-source-block-idom-maps.csv",
                "unique-idom-maps.txt",
            ],
            "redex-instrument-metadata.zip",
            "redex-instrument-checksum.txt",
        ),
        CompressionEntry(
            "Redex Class Sizes",
            lambda args: True,
            True,
            [],
            ["redex-class-sizes.csv"],
            None,
            None,
        ),
        CompressionEntry(
            "Redex Stats",
            lambda args: True,
            False,
            ["redex-stats.txt"],
            [],
            None,
            None,
        ),
    ]





def finalize_redex(state: State) -> None:
    _assert_val(state.lib_manager).__exit__(*sys.exc_info())

    repack_start_time = timer()

    _assert_val(state.unpack_manager).__exit__(*sys.exc_info())

    meta_file_dir = join(state.dex_dir, "meta/")
    assert os.path.isdir(meta_file_dir), "meta dir %s does not exist" % meta_file_dir

    resource_file_mapping = join(meta_file_dir, "resource-mapping.txt")
    if os.path.exists(resource_file_mapping):
        _assert_val(state.zip_manager).set_resource_file_mapping(resource_file_mapping)
    _assert_val(state.zip_manager).__exit__(*sys.exc_info())

    align_and_sign_output_apk(
        _assert_val(state.zip_manager).output_apk,
        state.args.out,
        # In dev mode, reset timestamps.
        state.args.reset_zip_timestamps or state.args.dev,
        state.args.sign,
        state.args.keystore,
        state.args.keyalias,
        state.args.keypass,
        state.args.ignore_zipalign,
        state.args.page_align_libs,
    )

    logging.debug(
        "Creating output APK finished in {:.2f} seconds".format(
            timer() - repack_start_time
        )
    )

    compress_entries(
        get_compression_list(),
        meta_file_dir,
        os.path.dirname(state.args.out),
        state.args,
    )

    copy_all_file_to_out_dir(
        meta_file_dir, state.args.out, "*", "all redex generated artifacts"
    )

    redex_stats_filename = state.config_dict.get("stats_output", "redex-stats.txt")
    redex_stats_file = join(dirname(meta_file_dir), redex_stats_filename)
    if isfile(redex_stats_file):
        with open(redex_stats_file, "r") as fr:
            apk_input_size = getsize(state.args.input_apk)
            apk_output_size = getsize(state.args.out)
            redex_stats_json = json.load(fr)
            redex_stats_json["input_stats"]["total_stats"][
                "num_compressed_apk_bytes"
            ] = apk_input_size
            redex_stats_json["output_stats"]["total_stats"][
                "num_compressed_apk_bytes"
            ] = apk_output_size
            update_redex_stats_file = join(
                dirname(state.args.out), redex_stats_filename
            )
            with open(update_redex_stats_file, "w") as fw:
                json.dump(redex_stats_json, fw)

    # Write invocation file
    with open(join(dirname(state.args.out), "redex.py-invocation.txt"), "w") as f:
        print("%s" % " ".join(map(shlex.quote, sys.argv)), file=f)

    copy_all_file_to_out_dir(
        state.dex_dir, state.args.out, "*.dot", "approximate shape graphs"
    )


def _init_logging(level_str: str) -> None:
    levels = {
        "critical": logging.CRITICAL,
        "error": logging.ERROR,
        "warn": logging.WARNING,
        "warning": logging.WARNING,
        "info": logging.INFO,
        "debug": logging.DEBUG,
    }
    level = levels[level_str]
    logging.basicConfig(
        level=level,
        format="[%(levelname)-8s] %(message)s",
    )


def run_redex_passthrough(
    args: argparse.Namespace,
    exception_formatter: ExceptionMessageFormatter,
    output_line_handler: typing.Optional[typing.Callable[[str], str]],
) -> None:
    assert args.outdir
    assert args.dex_files

    state = State(
        args=args,
        config_dict={},
        debugger=None,
        dex_dir=args.outdir,
        dexen=[],
        extracted_apk_dir=None,
        stop_pass_idx=-1,
        lib_manager=None,
        unpack_manager=None,
        zip_manager=None,
    )
    run_redex_binary(state, exception_formatter, output_line_handler)


def run_redex(
    args: argparse.Namespace,
    exception_formatter: typing.Optional[ExceptionMessageFormatter] = None,
    output_line_handler: typing.Optional[typing.Callable[[str], str]] = None,
) -> None:
    # This is late, but hopefully early enough.
    _init_logging(args.log_level)

    with BuckConnectionScope():
        if exception_formatter is None:
            exception_formatter = ExceptionMessageFormatter()

        if args.outdir or args.dex_files:
            run_redex_passthrough(args, exception_formatter, output_line_handler)
            return
        else:
            assert args.input_apk

        with BuckPartScope("redex::Preparing", "Prepare to run redex"):
            state = prepare_redex(args)

        with BuckPartScope("redex::Run redex-all", "Actually run redex binary"):
            run_redex_binary(state, exception_formatter, output_line_handler)

        if args.stop_pass:
            # Do not remove temp dirs
            sys.exit()

        finalize_redex(state)


if __name__ == "__main__":
    keys: typing.Dict[str, str] = {}
    try:
        keystore: str = join(os.environ["HOME"], ".android", "debug.keystore")
        if isfile(keystore):
            keys["keystore"] = keystore
            keys["keyalias"] = "androiddebugkey"
            keys["keypass"] = "android"
    except Exception:
        pass
    args: argparse.Namespace = arg_parser(**keys).parse_args()
    validate_args(args)
    with_temp_cleanup(lambda: run_redex(args), args.always_clean_up)
