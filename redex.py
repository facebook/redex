#!/usr/bin/env python

# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import argparse
import distutils.version
import errno
import fnmatch
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import timeit
import zipfile

from os.path import abspath, basename, dirname, isdir, isfile, join
from pipes import quote

import pyredex.logger as logger
import pyredex.unpacker as unpacker
from pyredex.utils import abs_glob, make_temp_dir, remove_comments, remove_temp_dirs, sign_apk
from pyredex.logger import log


def patch_zip_file():
    # See http://bugs.python.org/issue14315
    old_decode_extra = zipfile.ZipInfo._decodeExtra

    def decodeExtra(self):
        try:
            old_decode_extra(self)
        except struct.error:
            pass
    zipfile.ZipInfo._decodeExtra = decodeExtra


timer = timeit.default_timer

per_file_compression = {}


def find_android_build_tools():
    VERSION_REGEXP = '\d+\.\d+(\.\d+)$'
    android_home = os.environ['ANDROID_SDK']
    build_tools = join(android_home, 'build-tools')
    version = max(
        (d for d in os.listdir(build_tools) if re.match(VERSION_REGEXP, d)),
        key=distutils.version.StrictVersion
    )
    return join(build_tools, version)


def pgize(name):
    return name.strip()[1:][:-1].replace("/", ".")


def write_debugger_commands(args):
    """
    Write out a shell script that allows us to rerun redex-all under gdb.
    """
    fd, gdb_script_name = tempfile.mkstemp(suffix='.sh', prefix='redex-gdb-')
    with os.fdopen(fd, 'w') as f:
        f.write('cd %s\n' % quote(os.getcwd()))
        f.write('gdb --args ')
        f.write(' '.join(map(quote, args)))
        os.fchmod(fd, 0o775)

    fd, lldb_script_name = tempfile.mkstemp(suffix='.sh', prefix='redex-lldb-')
    with os.fdopen(fd, 'w') as f:
        f.write('cd %s\n' % quote(os.getcwd()))
        f.write('lldb -- ')
        f.write(' '.join(map(quote, args)))
        os.fchmod(fd, 0o775)

    return {
        'gdb_script_name': gdb_script_name,
        'lldb_script_name': lldb_script_name,
    }


def add_extra_environment_args(env):
    # If we're running with ASAN, we'll want these flags, if we're not, they do
    # nothing
    if 'ASAN_OPTIONS' not in env:  # don't overwrite user specified options
        # We ignore leaks because they are high volume and low danger (for a
        # short running program like redex).
        # We don't detect container overflow because it finds bugs in our
        # libraries (namely jsoncpp and boost).
        env['ASAN_OPTIONS'] = 'detect_leaks=0:detect_container_overflow=0'

    # If we haven't set MALLOC_CONF but we have requested to profile the memory
    # of a specific pass, set some reasonable defaults
    if 'MALLOC_PROFILE_PASS' in env and 'MALLOC_CONF' not in env:
        env['MALLOC_CONF'] = 'prof:true,prof_prefix:jeprof.out,prof_gdump:true,prof_active:false'


def get_stop_pass_idx(passes_list, pass_name_and_num):
    # Get the stop position
    # pass_name_and num may be "MyPass#0", "MyPass#3" or "MyPass"
    pass_name = pass_name_and_num
    pass_order = 0
    if '#' in pass_name_and_num:
        pass_name, pass_order = pass_name_and_num.split('#', 1)
        try:
            pass_order = int(pass_order)
        except ValueError:
            sys.exit("Invalid stop-pass %s, should be in 'SomePass(#num)'" %
                     pass_name_and_num)
    cur_order = 0
    for _idx, _name in enumerate(passes_list):
        if _name == pass_name:
            if cur_order == pass_order:
                return _idx
            else:
                cur_order += 1
    sys.exit("Invalid stop-pass %s. %d %s in passes_list" %
             (pass_name_and_num, cur_order, pass_name))


def run_redex_binary(state):
    if state.args.redex_binary is None:
        try:
            state.args.redex_binary = subprocess.check_output(['which', 'redex-all']
                                                              ).rstrip().decode('ascii')
        except subprocess.CalledProcessError:
            pass
    if state.args.redex_binary is None:
        # __file__ can be /path/fb-redex.pex/redex.pyc
        dir_name = dirname(abspath(__file__))
        while not isdir(dir_name):
            dir_name = dirname(dir_name)
        state.args.redex_binary = join(dir_name, 'redex-all')
    if not isfile(state.args.redex_binary) or not os.access(state.args.redex_binary, os.X_OK):
        sys.exit('redex-all is not found or is not executable: ' +
                 state.args.redex_binary)
    log('Running redex binary at ' + state.args.redex_binary)

    args = [state.args.redex_binary] + [
        '--apkdir', state.extracted_apk_dir,
        '--outdir', state.dex_dir]
    if state.args.config:
        args += ['--config', state.args.config]

    if state.args.verify_none_mode or state.config_dict.get("verify_none_mode"):
        args += ['--verify-none-mode']

    if state.args.is_art_build:
        args += ['--is-art-build']

    if state.args.enable_instrument_pass or state.config_dict.get("enable_instrument_pass"):
        args += ['--enable-instrument-pass']

    if state.args.warn:
        args += ['--warn', state.args.warn]
    args += ['--proguard-config=' + x for x in state.args.proguard_configs]
    if state.args.proguard_map:
        args += ['-Sproguard_map=' + state.args.proguard_map]

    args += ['--jarpath=' + x for x in state.args.jarpaths]
    if state.args.printseeds:
        args += ['--printseeds=' + state.args.printseeds]
    if state.args.used_js_assets:
        args += ['--used-js-assets=' + x for x in state.args.used_js_assets]
    args += ['-S' + x for x in state.args.passthru]
    args += ['-J' + x for x in state.args.passthru_json]

    args += state.dexen

    # Stop before a pass and output intermediate dex and IR meta data.
    if state.stop_pass_idx != -1:
        args += ['--stop-pass', str(state.stop_pass_idx),
                 '--output-ir', state.args.output_ir]

    if state.debugger == 'lldb':
        args = ['lldb', '--'] + args
    elif state.debugger == 'gdb':
        args = ['gdb', '--args'] + args

    start = timer()

    if state.args.debug:
        print('cd %s && %s' % (os.getcwd(), ' '.join(map(quote, args))))
        sys.exit()

    env = logger.setup_trace_for_child(os.environ)
    logger.flush()

    add_extra_environment_args(env)

    # Our CI system occasionally fails because it is trying to write the
    # redex-all binary when this tries to run.  This shouldn't happen, and
    # might be caused by a JVM bug.  Anyways, let's retry and hope it stops.
    for i in range(5):
        try:
            subprocess.check_call(args, env=env)
        except OSError as err:
            if err.errno == errno.ETXTBSY:
                if i < 4:
                    time.sleep(5)
                    continue
            raise err
        except subprocess.CalledProcessError as err:
            script_filenames = write_debugger_commands(args)
            raise RuntimeError(
                ('redex-all crashed with exit code %s! ' % err.returncode) +
                ('You can re-run it '
                 'under gdb by running %(gdb_script_name)s or under lldb '
                 'by running %(lldb_script_name)s') % script_filenames)
        break
    log('Dex processing finished in {:.2f} seconds'.format(timer() - start))


def extract_dex_number(dexfilename):
    m = re.search('(classes|.*-)(\d+)', basename(dexfilename))
    if m is None:
        raise Exception('Bad secondary dex name: ' + dexfilename)
    return int(m.group(2))


def dex_glob(directory):
    """
    Return the dexes in a given directory, with the primary dex first.
    """
    primary = join(directory, 'classes.dex')
    if not isfile(primary):
        raise Exception('No primary dex found')

    secondaries = [d for d in glob.glob(join(directory, '*.dex'))
                   if not d.endswith('classes.dex')]
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
        dirpath = join(root, 'dex' + str(idx))
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
        zipalign = [join(find_android_build_tools(), 'zipalign')]
    except Exception:
        # We couldn't find zipalign via ANDROID_SDK.  Try PATH.
        zipalign = ['zipalign']
    args = ['4', unaligned_apk_path, output_apk_path]
    if page_align:
        args = ['-p'] + args
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
                raise Exception('Zipalign failed to run')
            shutil.copy(unaligned_apk_path, output_apk_path)
    os.remove(unaligned_apk_path)


def create_output_apk(extracted_apk_dir, output_apk_path, sign, keystore,
                      key_alias, key_password, ignore_zipalign, page_align):

    # Remove old signature files
    for f in abs_glob(extracted_apk_dir, 'META-INF/*'):
        cert_path = join(extracted_apk_dir, f)
        if isfile(cert_path):
            os.remove(cert_path)

    directory = make_temp_dir('.redex_unaligned', False)
    unaligned_apk_path = join(directory, 'redex-unaligned.apk')

    if isfile(unaligned_apk_path):
        os.remove(unaligned_apk_path)

    # Create new zip file
    with zipfile.ZipFile(unaligned_apk_path, 'w') as unaligned_apk:
        for dirpath, _dirnames, filenames in os.walk(extracted_apk_dir):
            for filename in filenames:
                filepath = join(dirpath, filename)
                archivepath = filepath[len(extracted_apk_dir) + 1:]
                try:
                    compress = per_file_compression[archivepath]
                except KeyError:
                    compress = zipfile.ZIP_DEFLATED
                unaligned_apk.write(filepath, archivepath,
                                    compress_type=compress)

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


def merge_proguard_maps(
        redex_rename_map_path,
        input_apk_path,
        apk_output_path,
        dex_dir,
        pg_file):
    log('running merge proguard step')
    redex_rename_map_path = join(dex_dir, redex_rename_map_path)
    log('redex map is at ' + str(redex_rename_map_path))
    log('pg map is at ' + str(pg_file))
    assert os.path.isfile(redex_rename_map_path)
    redex_pg_file = "redex-class-rename-map.txt"
    output_dir = os.path.dirname(apk_output_path)
    output_file = join(output_dir, redex_pg_file)
    # If -dontobfuscate is set, proguard won't produce a mapping file, but
    # buck will create an empty mapping.txt. Check for this case.
    if pg_file and os.path.getsize(pg_file) > 0:
        update_proguard_mapping_file(
            pg_file,
            redex_rename_map_path,
            output_file)
        log('merging proguard map with redex class rename map')
        log('pg mapping file input is ' + str(pg_file))
        log('wrote redex pg format mapping file to ' + str(output_file))
    else:
        log('no proguard map file found')
        shutil.move(redex_rename_map_path, output_file)


def update_proguard_mapping_file(pg_map, redex_map, output_file):
    with open(pg_map, 'r') as pg_map,\
            open(redex_map, 'r') as redex_map,\
            open(output_file, 'w') as output:
        cls_regex = re.compile(r'^(.*) -> (.*):')
        redex_dict = {}
        for line in redex_map:
            match_obj = cls_regex.match(line)
            if match_obj:
                unmangled = match_obj.group(1)
                mangled = match_obj.group(2)
                redex_dict[unmangled] = mangled
        for line in pg_map:
            match_obj = cls_regex.match(line)
            if match_obj:
                unmangled = match_obj.group(1)
                mangled = match_obj.group(2)
                new_mapping = line.rstrip()
                if unmangled in redex_dict:
                    out_mangled = redex_dict.pop(unmangled)
                    new_mapping = unmangled + ' -> ' + out_mangled + ':'
                    print(new_mapping, file=output)
                else:
                    print(line.rstrip(), file=output)
            else:
                print(line.rstrip(), file=output)
        for unmangled, mangled in redex_dict.items():
            print('%s -> %s:' % (unmangled, mangled), file=output)


def overwrite_proguard_maps(
        redex_rename_map_path,
        apk_output_path,
        dex_dir,
        pg_file):
    log('running overwrite proguard step')
    redex_rename_map_path = join(dex_dir, redex_rename_map_path)
    log('redex map is at ' + str(redex_rename_map_path))
    log('pg map is at ' + str(pg_file))
    assert os.path.isfile(redex_rename_map_path)
    redex_pg_file = "redex-class-rename-map.txt"
    output_dir = os.path.dirname(apk_output_path)
    output_file = join(output_dir, redex_pg_file)
    log('wrote redex pg format mapping file to ' + str(output_file))
    shutil.move(redex_rename_map_path, output_file)


def copy_file_to_out_dir(tmp, apk_output_path, name, human_name, out_name):
    output_dir = os.path.dirname(apk_output_path)
    output_path = os.path.join(output_dir, out_name)
    tmp_path = tmp + '/' + name
    if os.path.isfile(tmp_path):
        subprocess.check_call(['cp', tmp_path, output_path])
        log('Copying ' + human_name + ' map to output dir')
    else:
        log('Skipping ' + human_name + ' copy, since no file found to copy')


def copy_all_file_to_out_dir(tmp, apk_output_path, ext, human_name):
    tmp_path = tmp + '/' + ext
    for file in glob.glob(tmp_path):
        filename = os.path.basename(file)
        copy_file_to_out_dir(tmp, apk_output_path, filename,
                             human_name + " " + filename, filename)


def validate_args(args):
    if args.sign:
        for arg_name in ['keystore', 'keyalias', 'keypass']:
            if getattr(args, arg_name) is None:
                raise argparse.ArgumentTypeError(
                    'Could not find a suitable default for --{} and no value '
                    'was provided.  This argument is required when --sign '
                    'is used'.format(arg_name),
                )


def arg_parser(
        binary=None,
        config=None,
        keystore=None,
        keyalias=None,
        keypass=None,
):
    description = """
Given an APK, produce a better APK!

"""
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=description)

    parser.add_argument('input_apk', help='Input APK file')
    parser.add_argument('-o', '--out', nargs='?', type=os.path.realpath,
                        default='redex-out.apk',
                        help='Output APK file name (defaults to redex-out.apk)')
    parser.add_argument('-j', '--jarpath', dest='jarpaths', action='append', default=[],
                        help='Path to dependent library jar file')

    parser.add_argument('--redex-binary', nargs='?', default=binary,
                        help='Path to redex binary')

    parser.add_argument('-c', '--config', default=config,
                        help='Configuration file')

    parser.add_argument('--sign', action='store_true',
                        help='Sign the apk after optimizing it')
    parser.add_argument('-s', '--keystore', nargs='?', default=keystore)
    parser.add_argument('-a', '--keyalias', nargs='?', default=keyalias)
    parser.add_argument('-p', '--keypass', nargs='?', default=keypass)

    parser.add_argument('-u', '--unpack-only', action='store_true',
                        help='Unpack the apk and print the unpacked directories, don\'t '
                        'run any redex passes or repack the apk')

    parser.add_argument('--unpack-dest', nargs=1,
                        help='Specify the base name of the destination directories; works with -u')

    parser.add_argument('-w', '--warn', nargs='?',
                        help='Control verbosity of warnings')

    parser.add_argument('-d', '--debug', action='store_true',
                        help='Unpack the apk and print the redex command line to run')

    parser.add_argument('--dev', action='store_true',
                        help='Optimize for development speed')

    parser.add_argument('-m', '--proguard-map', nargs='?',
                        help='Path to proguard mapping.txt for deobfuscating names')

    parser.add_argument('-q', '--printseeds', nargs='?',
                        help='File to print seeds to')

    parser.add_argument('--used-js-assets', action='append', default=[],
                        help='A JSON file (or files) containing a list of resources used by JS')

    parser.add_argument('-P', '--proguard-config', dest='proguard_configs',
                        action='append', default=[], help='Path to proguard config')

    parser.add_argument('-k', '--keep', nargs='?',
                        help='[deprecated] Path to file containing classes to keep')

    parser.add_argument('-S', dest='passthru', action='append', default=[],
                        help='Arguments passed through to redex')
    parser.add_argument('-J', dest='passthru_json', action='append', default=[],
                        help='JSON-formatted arguments passed through to redex')

    parser.add_argument('--lldb', action='store_true',
                        help='Run redex binary in lldb')
    parser.add_argument('--gdb', action='store_true',
                        help='Run redex binary in gdb')
    parser.add_argument('--ignore-zipalign', action='store_true',
                        help='Ignore if zipalign is not found')
    parser.add_argument('--verify-none-mode', action='store_true',
                        help='Enable verify-none mode on redex')
    parser.add_argument('--enable-instrument-pass', action='store_true',
                        help='Enable InstrumentPass if any')
    parser.add_argument('--is-art-build', action='store_true',
                        help='States that this is an art only build')
    parser.add_argument('--page-align-libs', action='store_true',
                        help='Preserve 4k page alignment for uncompressed libs')

    parser.add_argument('--side-effect-summaries',
                        help='Side effect information for external methods')

    parser.add_argument('--escape-summaries',
                        help='Escape information for external methods')

    parser.add_argument('--stop-pass', default='',
                        help='Stop before a pass and dump intermediate dex and IR meta data to a directory')
    parser.add_argument('--output-ir', default='',
                        help='Stop before stop_pass and dump intermediate dex and IR meta data to output_ir folder')
    return parser


class State(object):
    # This structure is only used for passing arguments between prepare_redex,
    # launch_redex_binary, finalize_redex
    def __init__(self, application_modules, args, config_dict, debugger,
                 dex_dir, dexen, dex_mode, extracted_apk_dir, temporary_libs_dir,
                 stop_pass_idx):
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
        assert get_file_ext(args.input_apk) == get_file_ext(args.out),\
            "Input file extension (\"" +\
            get_file_ext(args.input_apk) +\
            "\") should be the same as output file extension (\"" +\
            get_file_ext(args.out) + "\")"

    extracted_apk_dir = None
    dex_dir = None
    if args.unpack_only and args.unpack_dest:
        if args.unpack_dest[0] == '.':
            # Use APK's name
            unpack_dir_basename = os.path.splitext(args.input_apk)[0]
        else:
            unpack_dir_basename = args.unpack_dest[0]
        extracted_apk_dir = unpack_dir_basename + '.redex_extracted_apk'
        dex_dir = unpack_dir_basename + '.redex_dexen'
        try:
            os.makedirs(extracted_apk_dir)
            os.makedirs(dex_dir)
            extracted_apk_dir = os.path.abspath(extracted_apk_dir)
            dex_dir = os.path.abspath(dex_dir)
        except OSError as e:
            if e.errno == errno.EEXIST:
                print('Error: destination directory already exists!')
                print('APK: ' + extracted_apk_dir)
                print('DEX: ' + dex_dir)
                sys.exit(1)
            raise e

    config = args.config
    binary = args.redex_binary
    log('Using config ' + (config if config is not None else '(default)'))
    log('Using binary ' + (binary if binary is not None else '(default)'))

    if config is None:
        config_dict = {}
    else:
        with open(config) as config_file:
            try:
                lines = config_file.readlines()
                config_dict = json.loads(remove_comments(lines))
            except ValueError:
                raise ValueError("Invalid JSON in ReDex config file: %s" %
                                 config_file.name)

    # stop_pass_idx >= 0 means need stop before a pass and dump intermediate result
    stop_pass_idx = -1
    if args.stop_pass:
        passes_list = config_dict.get('redex', {}).get('passes', [])
        stop_pass_idx = get_stop_pass_idx(passes_list, args.stop_pass)
        if not args.output_ir or isfile(args.output_ir):
            print('Error: output_ir should be a directory')
            sys.exit(1)
        try:
            os.makedirs(args.output_ir)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise e

    unpack_start_time = timer()
    if not extracted_apk_dir:
        extracted_apk_dir = make_temp_dir('.redex_extracted_apk', debug_mode)

    log('Extracting apk...')
    unzip_apk(args.input_apk, extracted_apk_dir)

    dex_file_path = get_dex_file_path(args, extracted_apk_dir)

    dex_mode = unpacker.detect_secondary_dex_mode(dex_file_path)
    log('Detected dex mode ' + str(type(dex_mode).__name__))
    if not dex_dir:
        dex_dir = make_temp_dir('.redex_dexen', debug_mode)

    log('Unpacking dex files')
    dex_mode.unpackage(dex_file_path, dex_dir)

    log('Detecting Application Modules')
    application_modules = unpacker.ApplicationModule.detect(extracted_apk_dir)
    store_files = []
    store_metadata_dir = make_temp_dir(
        '.application_module_metadata', debug_mode)
    for module in application_modules:
        canary_prefix = module.get_canary_prefix()
        log('found module: ' + module.get_name() + ' ' +
            (canary_prefix if canary_prefix is not None else '(no canary prefix)'))
        store_path = os.path.join(dex_dir, module.get_name())
        os.mkdir(store_path)
        module.unpackage(extracted_apk_dir, store_path)
        store_metadata = os.path.join(
            store_metadata_dir, module.get_name() + '.json')
        module.write_redex_metadata(store_path, store_metadata)
        store_files.append(store_metadata)

    # Some of the native libraries can be concatenated together into one
    # xz-compressed file. We need to decompress that file so that we can scan
    # through it looking for classnames.
    libs_to_extract = []
    temporary_libs_dir = None
    xz_lib_name = 'libs.xzs'
    zstd_lib_name = 'libs.zstd'
    for root, _, filenames in os.walk(extracted_apk_dir):
        for filename in fnmatch.filter(filenames, xz_lib_name):
            libs_to_extract.append(join(root, filename))
        for filename in fnmatch.filter(filenames, zstd_lib_name):
            fullpath = join(root, filename)
            # For voltron modules BUCK creates empty zstd files for each module
            if os.path.getsize(fullpath) > 0:
                libs_to_extract.append(fullpath)
    if len(libs_to_extract) > 0:
        libs_dir = join(extracted_apk_dir, 'lib')
        extracted_dir = join(libs_dir, '__extracted_libs__')
        # Ensure both directories exist.
        temporary_libs_dir = ensure_libs_dir(libs_dir, extracted_dir)
        lib_count = 0
        for lib_to_extract in libs_to_extract:
            extract_path = join(extracted_dir, "lib_{}.so".format(lib_count))
            if lib_to_extract.endswith(xz_lib_name):
                cmd = 'xz -d --stdout {} > {}'.format(
                    lib_to_extract, extract_path)
            else:
                cmd = 'zstd -d {} -o {}'.format(lib_to_extract, extract_path)
            subprocess.check_call(cmd, shell=True)
            lib_count += 1

    if args.unpack_only:
        print('APK: ' + extracted_apk_dir)
        print('DEX: ' + dex_dir)
        sys.exit()

    # Move each dex to a separate temporary directory to be operated by
    # redex.
    dexen = move_dexen_to_directories(dex_dir, dex_glob(dex_dir))
    for store in store_files:
        dexen.append(store)
    log('Unpacking APK finished in {:.2f} seconds'.format(
        timer() - unpack_start_time))

    if args.side_effect_summaries is not None:
        args.passthru_json.append(
            'DeadCodeEliminationPass.side_effect_summaries="%s"' % args.side_effect_summaries
        )

    if args.escape_summaries is not None:
        args.passthru_json.append(
            'DeadCodeEliminationPass.escape_summaries="%s"' % args.escape_summaries
        )

    for key_value_str in args.passthru_json:
        key_value = key_value_str.split('=', 1)
        if len(key_value) != 2:
            log("Json Pass through %s is not valid. Split len: %s" %
                (key_value_str, len(key_value)))
            continue
        key = key_value[0]
        value = key_value[1]
        prev_value = config_dict.get(key, "(No previous value)")
        log("Got Override %s = %s from %s. Previous %s" %
            (key, value, key_value_str, prev_value))
        config_dict[key] = value

    log('Running redex-all on {} dex files '.format(len(dexen)))
    if args.lldb:
        debugger = 'lldb'
    elif args.gdb:
        debugger = 'gdb'
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
        stop_pass_idx=stop_pass_idx)


def finalize_redex(state):
    # This dir was just here so we could scan it for classnames, but we don't
    # want to pack it back up into the apk
    if state.temporary_libs_dir is not None:
        shutil.rmtree(state.temporary_libs_dir)

    repack_start_time = timer()

    log('Repacking dex files')
    have_locators = state.config_dict.get("emit_locator_strings")
    have_name_based_locators = state.config_dict.get("emit_name_based_locator_strings")
    log("Emit Locator Strings: %s" % have_locators)
    log("Emit Name Based Locator Strings: %s" % have_name_based_locators)

    state.dex_mode.repackage(
        get_dex_file_path(state.args, state.extracted_apk_dir), state.dex_dir, have_locators, have_name_based_locators, fast_repackage=state.args.dev
    )

    locator_store_id = 1
    for module in state.application_modules:
        log('repacking module: ' + module.get_name() +
            ' with id ' + str(locator_store_id))
        module.repackage(
            state.extracted_apk_dir, state.dex_dir, have_locators, have_name_based_locators, locator_store_id,
            fast_repackage=state.args.dev
        )
        locator_store_id = locator_store_id + 1

    log('Creating output apk')
    create_output_apk(state.extracted_apk_dir, state.args.out, state.args.sign, state.args.keystore,
                      state.args.keyalias, state.args.keypass, state.args.ignore_zipalign, state.args.page_align_libs)
    log('Creating output APK finished in {:.2f} seconds'.format(
        timer() - repack_start_time))
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'redex-line-number-map', 'line number map', 'redex-line-number-map')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'redex-line-number-map-v2',
                         'line number map v2', 'redex-line-number-map-v2')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'stats.txt', 'stats', 'redex-stats.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'timings.txt', 'timings', 'redex-timings.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'filename_mappings.txt',
                         'src strings map', 'redex-src-strings-map.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'outliner-artifacts.bin',
                         'outliner artifacts', 'redex-outliner-artifacts.bin')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'method_mapping.txt', 'method id map', 'redex-method-id-map.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'class_mapping.txt', 'class id map', 'redex-class-id-map.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'bytecode_offset_map.txt',
                         'bytecode offset map', 'redex-bytecode-offset-map.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'coldstart_fields_in_R_classes.txt',
                         'resources accessed during coldstart', 'redex-tracked-coldstart-resources.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'class_dependencies.txt', 'stats', 'redex-class-dependencies.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'resid-optres-mapping.json',
                         'resid map after optres pass', 'redex-resid-optres-mapping.json')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'resid-dedup-mapping.json',
                         'resid map after dedup pass', 'redex-resid-dedup-mapping.json')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'resid-splitres-mapping.json',
                         'resid map after split pass', 'redex-resid-splitres-mapping.json')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'type-erasure-mappings.txt',
                         'class map after type erasure pass', 'redex-type-erasure-mappings.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'instrument-metadata.txt',
                         'metadata file for instrumentation', 'redex-instrument-metadata.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'cleanup-removed-classes.txt',
                         'cleanup removed classes', 'redex-cleanup-removed-classes.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'unreachable-removed-symbols.txt',
                         'unreachable removed symbols', 'redex-unreachable-removed-symbols.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out,
                         'opt-decisions.json', 'opt info', 'redex-opt-decisions.json')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'redex-debug-line-map-v2',
                         'debug method id map', 'redex-debug-line-map-v2')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'class-method-info-map.txt',
                         'class method info map', 'redex-class-method-info-map.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'cfp-unsafe-references.txt',
                         'cfp unsafe references', 'redex-cfp-unsafe-references.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'merge-interface-mappings.txt',
                         'merged interface to merger interface', 'redex-merge-interface-mappings.txt')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'reachability-graph',
                         'reachability graph', 'redex-reachability-graph')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'method-override-graph',
                         'method override graph', 'redex-method-override-graph')
    copy_file_to_out_dir(state.dex_dir, state.args.out, 'iodi-metadata',
                         'iodi metadata', 'iodi-metadata')
    copy_all_file_to_out_dir(
        state.dex_dir, state.args.out, '*.dot', 'approximate shape graphs')

    if state.config_dict.get('proguard_map_output', '') != '':
        # if our map output strategy is overwrite, we don't merge at all
        # if you enable ObfuscatePass, this needs to be overwrite
        if state.config_dict.get('proguard_map_output_strategy', 'merge') == 'overwrite':
            overwrite_proguard_maps(
                state.config_dict['proguard_map_output'],
                state.args.out,
                state.dex_dir,
                state.args.proguard_map)
        else:
            merge_proguard_maps(
                state.config_dict['proguard_map_output'],
                state.args.input_apk,
                state.args.out,
                state.dex_dir,
                state.args.proguard_map)
    else:
        passes_list = state.config_dict.get('redex', {}).get('passes', [])
        assert 'RenameClassesPass' not in passes_list and\
            'RenameClassesPassV2' not in passes_list


def run_redex(args):
    state = prepare_redex(args)
    run_redex_binary(state)

    if args.stop_pass:
        # Do not remove temp dirs
        sys.exit()

    finalize_redex(state)
    remove_temp_dirs()


if __name__ == '__main__':
    patch_zip_file()
    keys = {}
    try:
        keystore = join(os.environ['HOME'], '.android', 'debug.keystore')
        if isfile(keystore):
            keys['keystore'] = keystore
            keys['keyalias'] = 'androiddebugkey'
            keys['keypass'] = 'android'
    except Exception:
        pass
    args = arg_parser(**keys).parse_args()
    validate_args(args)
    run_redex(args)
