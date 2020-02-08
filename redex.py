#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import distutils.version
import enum
import errno
import fnmatch
import glob
import itertools
import json
import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import timeit
import zipfile
from os.path import abspath, basename, dirname, isdir, isfile, join
from pipes import quote

import pyredex.logger as logger
import pyredex.unpacker as unpacker
from pyredex.logger import log
from pyredex.utils import (
    abs_glob,
    make_temp_dir,
    remove_comments,
    sign_apk,
    with_temp_cleanup,
)


def patch_zip_file():
    # See http://bugs.python.org/issue14315
    old_decode_extra = zipfile.ZipInfo._decodeExtra

    def decodeExtra(self):
        try:
            old_decode_extra(self)
        except struct.error:
            pass

    zipfile.ZipInfo._decodeExtra = decodeExtra


patch_zip_file()

timer = timeit.default_timer

per_file_compression = {}


def find_android_build_tools():
    VERSION_REGEXP = r"\d+\.\d+(\.\d+)$"
    android_home = os.environ["ANDROID_SDK"]
    build_tools = join(android_home, "build-tools")
    version = max(
        (d for d in os.listdir(build_tools) if re.match(VERSION_REGEXP, d)),
        key=distutils.version.StrictVersion,
    )
    return join(build_tools, version)


def pgize(name):
    return name.strip()[1:][:-1].replace("/", ".")


def dbg_prefix(dbg, src_root=None):
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
            cmd += ["-o", quote('settings set target.source-map "." "%s"' % src_root)]

    DBG_END = {"gdb": "--args", "lldb": "--"}
    cmd.append(DBG_END[dbg])

    return cmd


def write_debugger_command(dbg, src_root, args):
    """Write out a shell script that allows us to rerun redex-all under a debugger.

    The choice of debugger is governed by `dbg` which can be either "gdb" or "lldb".
    """
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
        os.fchmod(fd, 0o775)

    return script_name


def add_extra_environment_args(env):
    # If we haven't set MALLOC_CONF but we have requested to profile the memory
    # of a specific pass, set some reasonable defaults
    if "MALLOC_PROFILE_PASS" in env and "MALLOC_CONF" not in env:
        env[
            "MALLOC_CONF"
        ] = "prof:true,prof_prefix:jeprof.out,prof_gdump:true,prof_active:false"

    # If we haven't set MALLOC_CONF, tune MALLOC_CONF for better perf
    if "MALLOC_CONF" not in env:
        env["MALLOC_CONF"] = "background_thread:true,metadata_thp:always,thp:always"


def get_stop_pass_idx(passes_list, pass_name_and_num):
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


def maybe_addr2line(lines):
    backtrace_pattern = re.compile(r"^([^(]+)(?:\((.*)\))?\[(0x[0-9a-f]+)\]$")

    # Generate backtrace lines.
    def find_matches():
        for line in lines:
            stripped_line = line.strip()
            m = backtrace_pattern.fullmatch(stripped_line)
            if m is not None:
                yield m

    # Check whether there is anything to do.
    matches_gen = find_matches()
    first_elem = next(matches_gen, None)
    if first_elem is None:
        return
    matches_gen = itertools.chain([first_elem], matches_gen)

    # Check whether addr2line is available
    def has_addr2line():
        try:
            subprocess.check_call(
                ["addr2line", "-v"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            return False

    if not has_addr2line():
        sys.stderr.write("Addr2line not found!\n")
        return
    sys.stderr.write("\n")

    addr2line_base = ["addr2line", "-f", "-i", "-C", "-e"]

    def symbolize(filename, offset):
        # It's good enough not to use server mode.
        try:
            output = subprocess.check_output(addr2line_base + [filename, offset])
            return output.decode(sys.stderr.encoding).splitlines()
        except subprocess.CalledProcessError:
            return ["<addr2line error>"]

    for m in matches_gen:
        sys.stderr.write("%s(%s)[%s]\n" % (m.group(1), m.group(2), m.group(3)))
        decoded = symbolize(m.group(1), m.group(3))
        odd_line = True
        for line in decoded:
            sys.stderr.write("%s%s\n" % ("  " * (1 if odd_line else 2), line.strip()))
            odd_line = not odd_line

    sys.stderr.write("\n")


def run_and_stream_stderr(args, env, pass_fds):
    proc = subprocess.Popen(args, env=env, pass_fds=pass_fds, stderr=subprocess.PIPE)

    def stream_and_return():
        err_out = []
        # Copy and stash the output.
        for line in proc.stderr:
            try:
                str_line = line.decode(sys.stdout.encoding)
            except UnicodeDecodeError:
                str_line = "<UnicodeDecodeError>\n"
            sys.stderr.write(str_line)
            err_out.append(str_line)
            if len(err_out) > 1000:
                err_out = err_out[100:]

        returncode = proc.wait()

        return (returncode, err_out)

    return (proc, stream_and_return)


# Signal handlers.
# A SIGINT handler gives the process some time to wait for redex to terminate
# and symbolize a backtrace. The SIGINT handler uninstalls itself so that a
# second SIGINT really kills redex.py and install a SIGALRM handler. The SIGALRM
# handler either sends a SIGINT to redex, waits some more, or terminates
# redex.py.
class RedexState(enum.Enum):
    UNSTARTED = 1
    STARTED = 2
    POSTPROCESSING = 3
    FINISHED = 4


class SigIntHandler:
    def __init__(self):
        self._old_handler = None
        self._state = RedexState.UNSTARTED
        self._proc = None

    def install(self):
        # On Linux, support ctrl-c.
        # Note: must be on the main thread. Add checks. Portability is an issue.
        if platform.system() != "Linux":
            return

        self._old_handler = signal.getsignal(signal.SIGINT)
        signal.signal(signal.SIGINT, self._sigint_handler)

    def uninstall(self):
        if self._old_handler is not None:
            signal.signal(signal.SIGINT, self._old_handler)

    def set_state(self, new_state):
        self._state = new_state

    def set_proc(self, new_proc):
        self._proc = new_proc

    def _sigalrm_handler(self, _signum, _frame):
        signal.signal(signal.SIGALRM, signal.SIG_DFL)
        if self._state == RedexState.STARTED:
            # Send SIGINT in case redex-all was not in the same process
            # group and wait some more.
            self._proc.send_signal(signal.SIGINT)
            signal.alarm(3)
            return
        if self._state == RedexState.POSTPROCESSING:
            # Maybe symbolization took a while. Give it some more time.
            signal.alarm(3)
            return
        # Kill ourselves.
        os.kill(os.getpid(), signal.SIGINT)

    def _sigint_handler(self, _signum, _frame):
        signal.signal(signal.SIGINT, self._old_handler)
        if self._state == RedexState.UNSTARTED or self._state == RedexState.FINISHED:
            os.kill(os.getpid(), signal.SIGINT)

        # This is the first SIGINT, schedule some waiting period. redex-all is
        # likely in the same process group and already got a SIGINT delivered.
        signal.signal(signal.SIGALRM, self._sigalrm_handler)
        signal.alarm(3)


def run_redex_binary(state):
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
    log("Running redex binary at " + state.args.redex_binary)

    args = [state.args.redex_binary] + [
        "--apkdir",
        state.extracted_apk_dir,
        "--outdir",
        state.dex_dir,
    ]
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

    prefix = (
        dbg_prefix(state.debugger, state.args.debug_source_root)
        if state.debugger is not None
        else []
    )
    start = timer()

    if state.args.debug:
        print("cd %s && %s" % (os.getcwd(), " ".join(prefix + map(quote, args))))
        sys.exit()

    env = logger.setup_trace_for_child(os.environ)
    logger.flush()

    add_extra_environment_args(env)

    def run():
        sigint_handler = SigIntHandler()
        sigint_handler.install()

        try:
            proc, handler = run_and_stream_stderr(
                prefix + args, env, (logger.trace_fp.fileno(),)
            )
            sigint_handler.set_proc(proc)
            sigint_handler.set_state(RedexState.STARTED)

            returncode, err_out = handler()

            sigint_handler.set_state(RedexState.POSTPROCESSING)

            if returncode != 0:
                # Check for crash traces.
                maybe_addr2line(err_out)

                gdb_script_name = write_debugger_command(
                    "gdb", state.args.debug_source_root, args
                )
                lldb_script_name = write_debugger_command(
                    "lldb", state.args.debug_source_root, args
                )
                raise RuntimeError(
                    (
                        "redex-all crashed with exit code {}! You can re-run it "
                        + "under gdb by running {} or under lldb by running {}"
                    ).format(returncode, gdb_script_name, lldb_script_name)
                )
            return True
        except OSError as err:
            if err.errno == errno.ETXTBSY:
                return False
            raise err
        finally:
            sigint_handler.set_state(RedexState.FINISHED)
            sigint_handler.uninstall()

    # Our CI system occasionally fails because it is trying to write the
    # redex-all binary when this tries to run.  This shouldn't happen, and
    # might be caused by a JVM bug.  Anyways, let's retry and hope it stops.
    for _ in range(5):
        if run():
            break

    log("Dex processing finished in {:.2f} seconds".format(timer() - start))


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


def unzip_apk(apk, destination_directory):
    with zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            per_file_compression[info.filename] = info.compress_type
        z.extractall(destination_directory)


def zipalign(unaligned_apk_path, output_apk_path, ignore_zipalign, page_align):
    # Align zip and optionally perform good compression.
    try:
        zipalign = [join(find_android_build_tools(), "zipalign")]
    except Exception:
        # We couldn't find zipalign via ANDROID_SDK.  Try PATH.
        zipalign = ["zipalign"]
    args = ["4", unaligned_apk_path, output_apk_path]
    if page_align:
        args = ["-p"] + args
    success = False
    try:
        p = subprocess.Popen(zipalign + args, stderr=subprocess.PIPE)
        err = p.communicate()[1]
        if p.returncode != 0:
            error = err.decode(sys.getfilesystemencoding())
            print("Failed to execute zipalign, stderr: {}".format(error))
        else:
            success = True
    except OSError as e:
        if e.errno == errno.ENOENT:
            print("Couldn't find zipalign. See README.md to resolve this.")
        else:
            print("Failed to execute zipalign, strerror: {}".format(e.strerror))
    finally:
        if not success:
            if not ignore_zipalign:
                raise Exception("Zipalign failed to run")
            shutil.copy(unaligned_apk_path, output_apk_path)
    os.remove(unaligned_apk_path)


def create_output_apk(
    extracted_apk_dir,
    output_apk_path,
    sign,
    keystore,
    key_alias,
    key_password,
    ignore_zipalign,
    page_align,
):

    # Remove old signature files
    for f in abs_glob(extracted_apk_dir, "META-INF/*"):
        cert_path = join(extracted_apk_dir, f)
        if isfile(cert_path):
            os.remove(cert_path)

    directory = make_temp_dir(".redex_unaligned", False)
    unaligned_apk_path = join(directory, "redex-unaligned.apk")

    if isfile(unaligned_apk_path):
        os.remove(unaligned_apk_path)

    # Create new zip file
    with zipfile.ZipFile(unaligned_apk_path, "w") as unaligned_apk:
        for dirpath, _dirnames, filenames in os.walk(extracted_apk_dir):
            for filename in filenames:
                filepath = join(dirpath, filename)
                archivepath = filepath[len(extracted_apk_dir) + 1 :]
                try:
                    compress = per_file_compression[archivepath]
                except KeyError:
                    compress = zipfile.ZIP_DEFLATED
                unaligned_apk.write(filepath, archivepath, compress_type=compress)

    # Add new signature
    if sign:
        sign_apk(keystore, key_password, key_alias, unaligned_apk_path)

    if isfile(output_apk_path):
        os.remove(output_apk_path)

    try:
        os.makedirs(dirname(output_apk_path))
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise

    zipalign(unaligned_apk_path, output_apk_path, ignore_zipalign, page_align)


def copy_file_to_out_dir(tmp, apk_output_path, name, human_name, out_name):
    output_dir = os.path.dirname(apk_output_path)
    output_path = os.path.join(output_dir, out_name)
    tmp_path = tmp + "/" + name
    if os.path.isfile(tmp_path):
        subprocess.check_call(["cp", tmp_path, output_path])
        log("Copying " + human_name + " map to output dir")
    else:
        log("Skipping " + human_name + " copy, since no file found to copy")


def copy_all_file_to_out_dir(tmp, apk_output_path, ext, human_name):
    tmp_path = tmp + "/" + ext
    for file in glob.glob(tmp_path):
        filename = os.path.basename(file)
        copy_file_to_out_dir(
            tmp, apk_output_path, filename, human_name + " " + filename, filename
        )


def validate_args(args):
    if args.sign:
        for arg_name in ["keystore", "keyalias", "keypass"]:
            if getattr(args, arg_name) is None:
                raise argparse.ArgumentTypeError(
                    "Could not find a suitable default for --{} and no value "
                    "was provided.  This argument is required when --sign "
                    "is used".format(arg_name)
                )


def arg_parser(binary=None, config=None, keystore=None, keyalias=None, keypass=None):
    description = """
Given an APK, produce a better APK!

"""
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=description
    )

    parser.add_argument("input_apk", help="Input APK file")
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

    parser.add_argument(
        "--sign", action="store_true", help="Sign the apk after optimizing it"
    )
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

    parser.add_argument("-q", "--printseeds", nargs="?", help="File to print seeds to")

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

    return parser


class State(object):
    # This structure is only used for passing arguments between prepare_redex,
    # launch_redex_binary, finalize_redex
    def __init__(
        self,
        application_modules,
        args,
        config_dict,
        debugger,
        dex_dir,
        dexen,
        dex_mode,
        extracted_apk_dir,
        temporary_libs_dir,
        stop_pass_idx,
    ):
        self.application_modules = application_modules
        self.args = args
        self.config_dict = config_dict
        self.debugger = debugger
        self.dex_dir = dex_dir
        self.dexen = dexen
        self.dex_mode = dex_mode
        self.extracted_apk_dir = extracted_apk_dir
        self.temporary_libs_dir = temporary_libs_dir
        self.stop_pass_idx = stop_pass_idx


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


def get_dex_file_path(args, extracted_apk_dir):
    # base on file extension check if input is
    # an apk file (".apk") or an Android bundle file (".aab")
    # TODO: support loadable modules (at this point only
    # very basic support is provided - in case of Android bundles
    # "regular" apk file content is moved to the "base"
    # sub-directory of the bundle archive)
    if get_file_ext(args.input_apk) == ".aab":
        return join(extracted_apk_dir, "base", "dex")
    else:
        return extracted_apk_dir


def prepare_redex(args):
    debug_mode = args.unpack_only or args.debug

    # avoid accidentally mixing up file formats since we now support
    # both apk files and Android bundle files
    if not args.unpack_only:
        assert get_file_ext(args.input_apk) == get_file_ext(args.out), (
            'Input file extension ("'
            + get_file_ext(args.input_apk)
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
    log("Using config " + (config if config is not None else "(default)"))
    log("Using binary " + (binary if binary is not None else "(default)"))

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

    unpack_start_time = timer()
    if not extracted_apk_dir:
        extracted_apk_dir = make_temp_dir(".redex_extracted_apk", debug_mode)

    log("Extracting apk...")
    unzip_apk(args.input_apk, extracted_apk_dir)

    dex_file_path = get_dex_file_path(args, extracted_apk_dir)

    dex_mode = unpacker.detect_secondary_dex_mode(dex_file_path)
    log("Detected dex mode " + str(type(dex_mode).__name__))
    if not dex_dir:
        dex_dir = make_temp_dir(".redex_dexen", debug_mode)

    log("Unpacking dex files")
    dex_mode.unpackage(dex_file_path, dex_dir)

    log("Detecting Application Modules")
    application_modules = unpacker.ApplicationModule.detect(extracted_apk_dir)
    store_files = []
    store_metadata_dir = make_temp_dir(".application_module_metadata", debug_mode)
    for module in application_modules:
        canary_prefix = module.get_canary_prefix()
        log(
            "found module: "
            + module.get_name()
            + " "
            + (canary_prefix if canary_prefix is not None else "(no canary prefix)")
        )
        store_path = os.path.join(dex_dir, module.get_name())
        os.mkdir(store_path)
        module.unpackage(extracted_apk_dir, store_path)
        store_metadata = os.path.join(store_metadata_dir, module.get_name() + ".json")
        module.write_redex_metadata(store_path, store_metadata)
        store_files.append(store_metadata)

    # Some of the native libraries can be concatenated together into one
    # xz-compressed file. We need to decompress that file so that we can scan
    # through it looking for classnames.
    libs_to_extract = []
    temporary_libs_dir = None
    xz_lib_name = "libs.xzs"
    zstd_lib_name = "libs.zstd"
    for root, _, filenames in os.walk(extracted_apk_dir):
        for filename in fnmatch.filter(filenames, xz_lib_name):
            libs_to_extract.append(join(root, filename))
        for filename in fnmatch.filter(filenames, zstd_lib_name):
            fullpath = join(root, filename)
            # For voltron modules BUCK creates empty zstd files for each module
            if os.path.getsize(fullpath) > 0:
                libs_to_extract.append(fullpath)
    if len(libs_to_extract) > 0:
        libs_dir = join(extracted_apk_dir, "lib")
        extracted_dir = join(libs_dir, "__extracted_libs__")
        # Ensure both directories exist.
        temporary_libs_dir = ensure_libs_dir(libs_dir, extracted_dir)
        lib_count = 0
        for lib_to_extract in libs_to_extract:
            extract_path = join(extracted_dir, "lib_{}.so".format(lib_count))
            if lib_to_extract.endswith(xz_lib_name):
                cmd = "xz -d --stdout {} > {}".format(lib_to_extract, extract_path)
            else:
                cmd = "zstd -d {} -o {}".format(lib_to_extract, extract_path)
            subprocess.check_call(cmd, shell=True)
            lib_count += 1

    if args.unpack_only:
        print("APK: " + extracted_apk_dir)
        print("DEX: " + dex_dir)
        sys.exit()

    # Move each dex to a separate temporary directory to be operated by
    # redex.
    dexen = move_dexen_to_directories(dex_dir, dex_glob(dex_dir))
    for store in sorted(store_files):
        dexen.append(store)
    log("Unpacking APK finished in {:.2f} seconds".format(timer() - unpack_start_time))

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
            log(
                "Json Pass through %s is not valid. Split len: %s"
                % (key_value_str, len(key_value))
            )
            continue
        key = key_value[0]
        value = key_value[1]
        prev_value = config_dict.get(key, "(No previous value)")
        log(
            "Got Override %s = %s from %s. Previous %s"
            % (key, value, key_value_str, prev_value)
        )
        config_dict[key] = json.loads(value)

    log("Running redex-all on {} dex files ".format(len(dexen)))
    if args.lldb:
        debugger = "lldb"
    elif args.gdb:
        debugger = "gdb"
    else:
        debugger = None

    return State(
        application_modules=application_modules,
        args=args,
        config_dict=config_dict,
        debugger=debugger,
        dex_dir=dex_dir,
        dexen=dexen,
        dex_mode=dex_mode,
        extracted_apk_dir=extracted_apk_dir,
        temporary_libs_dir=temporary_libs_dir,
        stop_pass_idx=stop_pass_idx,
    )


def finalize_redex(state):
    # This dir was just here so we could scan it for classnames, but we don't
    # want to pack it back up into the apk
    if state.temporary_libs_dir is not None:
        shutil.rmtree(state.temporary_libs_dir)

    repack_start_time = timer()

    log("Repacking dex files")
    have_locators = state.config_dict.get("emit_locator_strings")
    log("Emit Locator Strings: %s" % have_locators)

    state.dex_mode.repackage(
        get_dex_file_path(state.args, state.extracted_apk_dir),
        state.dex_dir,
        have_locators,
        fast_repackage=state.args.dev,
    )

    locator_store_id = 1
    for module in state.application_modules:
        log(
            "repacking module: "
            + module.get_name()
            + " with id "
            + str(locator_store_id)
        )
        module.repackage(
            state.extracted_apk_dir,
            state.dex_dir,
            have_locators,
            locator_store_id,
            fast_repackage=state.args.dev,
        )
        locator_store_id = locator_store_id + 1

    log("Creating output apk")
    create_output_apk(
        state.extracted_apk_dir,
        state.args.out,
        state.args.sign,
        state.args.keystore,
        state.args.keyalias,
        state.args.keypass,
        state.args.ignore_zipalign,
        state.args.page_align_libs,
    )
    log(
        "Creating output APK finished in {:.2f} seconds".format(
            timer() - repack_start_time
        )
    )

    meta_file_dir = join(state.dex_dir, "meta/")
    assert os.path.isdir(meta_file_dir), "meta dir %s does not exist" % meta_file_dir

    copy_all_file_to_out_dir(
        meta_file_dir, state.args.out, "*", "all redex generated artifacts"
    )

    copy_all_file_to_out_dir(
        state.dex_dir, state.args.out, "*.dot", "approximate shape graphs"
    )


def run_redex(args):
    state = prepare_redex(args)
    run_redex_binary(state)

    if args.stop_pass:
        # Do not remove temp dirs
        sys.exit()

    finalize_redex(state)


if __name__ == "__main__":
    keys = {}
    try:
        keystore = join(os.environ["HOME"], ".android", "debug.keystore")
        if isfile(keystore):
            keys["keystore"] = keystore
            keys["keyalias"] = "androiddebugkey"
            keys["keypass"] = "android"
    except Exception:
        pass
    args = arg_parser(**keys).parse_args()
    validate_args(args)
    with_temp_cleanup(lambda: run_redex(args), args.always_clean_up)
