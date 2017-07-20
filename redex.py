#!/usr/bin/env python3

# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import argparse
import distutils.version
import errno
import functools
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import timeit
import zipfile

from os.path import abspath, basename, dirname, getsize, isdir, isfile, join, \
        realpath, split

import pyredex.logger as logger
import pyredex.unpacker as unpacker
from pyredex.utils import abs_glob, make_temp_dir, remove_temp_dirs
from pyredex.logger import log

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
        f.write('gdb --args ')
        f.write(' '.join(args))
        os.fchmod(fd, 0o775)

    fd, lldb_script_name = tempfile.mkstemp(suffix='.sh', prefix='redex-lldb-')
    with os.fdopen(fd, 'w') as f:
        f.write('lldb -- ')
        f.write(' '.join(args))
        os.fchmod(fd, 0o775)

    return {
        'gdb_script_name': gdb_script_name,
        'lldb_script_name': lldb_script_name,
    }


def run_pass(
        executable_path,
        script_args,
        config_path,
        config_json,
        apk_dir,
        dex_dir,
        dexfiles,
        debugger,
        ):

    if executable_path is None:
        try:
            executable_path = subprocess.check_output(['which', 'redex-all']
                                                     ).rstrip().decode('ascii')
        except subprocess.CalledProcessError:
            pass
    if executable_path is None:
        # __file__ can be /path/fb-redex.pex/redex.pyc
        dir_name = dirname(abspath(__file__))
        while not isdir(dir_name):
            dir_name = dirname(dir_name)
        executable_path = join(dir_name, 'redex-all')
    if not isfile(executable_path) or not os.access(executable_path, os.X_OK):
        sys.exit('redex-all is not found or is not executable')
    log('Running redex binary at ' + executable_path)

    args = [executable_path] + [
        '--apkdir', apk_dir,
        '--outdir', dex_dir]
    if config_path:
        args += ['--config', config_path]
    if script_args.warn:
        args += ['--warn', script_args.warn]
    args += ['--proguard-config=' + x for x in script_args.proguard_configs]
    if script_args.keep:
        args += ['--seeds', script_args.keep]
    if script_args.proguard_map:
        args += ['-Sproguard_map=' + script_args.proguard_map]

    args += ['--jarpath=' + x for x in script_args.jarpaths]
    if script_args.printseeds:
        args += ['--printseeds=' + script_args.printseeds]
    args += ['-S' + x for x in script_args.passthru]
    args += ['-J' + x for x in script_args.passthru_json]

    args += dexfiles

    if debugger == 'lldb':
        args = ['lldb', '--'] + args
    elif debugger == 'gdb':
        args = ['gdb', '--args'] + args

    start = timer()

    if script_args.debug:
        print(' '.join(args))
        sys.exit()

    env = logger.setup_trace_for_child(os.environ)
    logger.flush()

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


def zipalign(unaligned_apk_path, output_apk_path, ignore_zipalign):
    # Align zip and optionally perform good compression.
    try:
        zipalign = [join(find_android_build_tools(), 'zipalign')]
    except:
        # We couldn't find zipalign via ANDROID_SDK.  Try PATH.
        zipalign = ['zipalign']
    try:
        subprocess.check_call(zipalign +
                              ['4', unaligned_apk_path, output_apk_path])
    except:
        print("Couldn't find zipalign. See README.md to resolve this.")
        if not ignore_zipalign:
            raise Exception('No zipalign executable found')
        shutil.copy(unaligned_apk_path, output_apk_path)
    os.remove(unaligned_apk_path)


def create_output_apk(extracted_apk_dir, output_apk_path, sign, keystore,
        key_alias, key_password, ignore_zipalign):

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
        for dirpath, dirnames, filenames in os.walk(extracted_apk_dir):
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
        subprocess.check_call([
                'jarsigner',
                '-sigalg', 'SHA1withRSA',
                '-digestalg', 'SHA1',
                '-keystore', keystore,
                '-storepass', key_password,
                unaligned_apk_path, key_alias],
            stdout=sys.stderr)

    if isfile(output_apk_path):
        os.remove(output_apk_path)

    zipalign(unaligned_apk_path, output_apk_path, ignore_zipalign)


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
        for unmangled, mangled in redex_dict.iteritems():
            print('%s -> %s:' % (unmangled, mangled), file=output)


def copy_file_to_out_dir(tmp, apk_output_path, name, human_name, out_name):
    output_dir = os.path.dirname(apk_output_path)
    output_path = os.path.join(output_dir, out_name)
    tmp_path = tmp + '/' + name
    if os.path.isfile(tmp_path):
        subprocess.check_call(['cp', tmp_path, output_path])
        log('Copying ' + human_name + ' map to output dir')
    else:
        log('Skipping ' + human_name + ' copy, since no file found to copy')


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
    parser.add_argument('-o', '--out', nargs='?', default='redex-out.apk',
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

    parser.add_argument('-w', '--warn', nargs='?',
            help='Control verbosity of warnings')

    parser.add_argument('-d', '--debug', action='store_true',
            help='Unpack the apk and print the redex command line to run')

    parser.add_argument('-m', '--proguard-map', nargs='?',
            help='Path to proguard mapping.txt for deobfuscating names')

    parser.add_argument('-q', '--printseeds', nargs='?',
            help='File to print seeds to')

    parser.add_argument('-P', '--proguard-config', dest='proguard_configs',
            action='append', default=[], help='Path to proguard config')

    parser.add_argument('-k', '--keep', nargs='?',
            help='Path to file containing classes to keep')

    parser.add_argument('-S', dest='passthru', action='append', default=[],
            help='Arguments passed through to redex')
    parser.add_argument('-J', dest='passthru_json', action='append', default=[],
            help='JSON-formatted arguments passed through to redex')

    parser.add_argument('--lldb', action='store_true', help='Run redex binary in lldb')
    parser.add_argument('--gdb', action='store_true', help='Run redex binary in gdb')
    parser.add_argument('--ignore-zipalign', action='store_true', help='Ignore if zipalign is not found')

    return parser


def run_redex(args):
    debug_mode = args.unpack_only or args.debug

    config = args.config
    binary = args.redex_binary
    log('Using config ' + (config if config is not None else '(default)'))
    log('Using binary ' + (binary if binary is not None else '(default)'))

    if config is None:
        config_dict = {}
        passes_list = []
    else:
        with open(config) as config_file:
            try:
                config_dict = json.load(config_file)
            except ValueError:
                raise ValueError("Invalid JSON in ReDex config file: %s" %
                                 config_file.name)
            passes_list = config_dict['redex']['passes']

    unpack_start_time = timer()
    extracted_apk_dir = make_temp_dir('.redex_extracted_apk', debug_mode)

    log('Extracting apk...')
    unzip_apk(args.input_apk, extracted_apk_dir)

    dex_mode = unpacker.detect_secondary_dex_mode(extracted_apk_dir)
    log('Detected dex mode ' + str(type(dex_mode).__name__))
    dex_dir = make_temp_dir('.redex_dexen', debug_mode)

    log('Unpacking dex files')
    dex_mode.unpackage(extracted_apk_dir, dex_dir)

    log('Detecting Application Modules')
    application_modules = unpacker.ApplicationModule.detect(extracted_apk_dir)
    store_files = []
    store_metadata_dir = make_temp_dir('.application_module_metadata', debug_mode)
    for module in application_modules:
        log('found module: ' + module.get_name() + ' ' + module.get_canary_prefix())
        store_path = os.path.join(dex_dir, module.get_name())
        os.mkdir(store_path)
        module.unpackage(extracted_apk_dir, store_path)
        store_metadata = os.path.join(store_metadata_dir, module.get_name() + '.json')
        module.write_redex_metadata(store_path, store_metadata)
        store_files.append(store_metadata)

    # Some of the native libraries can be concatenated together into one
    # xz-compressed file. We need to decompress that file so that we can scan
    # through it looking for classnames.
    xz_compressed_libs = join(extracted_apk_dir, 'assets/lib/libs.xzs')
    temporary_lib_file = join(extracted_apk_dir, 'lib/concated_native_libs.so')
    if os.path.exists(xz_compressed_libs):
        cmd = 'xz -d --stdout {} > {}'.format(xz_compressed_libs, temporary_lib_file)
        subprocess.check_call(cmd, shell=True)

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

    for key_value_str in args.passthru_json:
        key_value = key_value_str.split('=', 1)
        if len(key_value) != 2:
            log("Json Pass through %s is not valid. Split len: %s" % (key_value_str, len(key_value)))
            continue
        key = key_value[0]
        value = key_value[1]
        log("Got Override %s = %s from %s. Previous %s" % (key, value, key_value_str, config_dict[key]))
        config_dict[key] = value

    log('Running redex-all on {} dex files '.format(len(dexen)))
    if args.lldb:
        debugger = 'lldb'
    elif args.gdb:
        debugger = 'gdb'
    else:
        debugger = None

    run_pass(binary,
             args,
             config,
             config_dict,
             extracted_apk_dir,
             dex_dir,
             dexen,
             debugger)

    # This file was just here so we could scan it for classnames, but we don't
    # want to pack it back up into the apk
    if os.path.exists(temporary_lib_file):
        os.remove(temporary_lib_file)

    repack_start_time = timer()

    log('Repacking dex files')
    have_locators = config_dict.get("emit_locator_strings")
    log("Emit Locator Strings: %s" % have_locators)

    dex_mode.repackage(extracted_apk_dir, dex_dir, have_locators)

    locator_store_id = 1
    for module in application_modules:
        log('repacking module: ' + module.get_name() + ' with id ' + str(locator_store_id))
        module.repackage(extracted_apk_dir, dex_dir, have_locators, locator_store_id)
        locator_store_id = locator_store_id + 1

    log('Creating output apk')
    create_output_apk(extracted_apk_dir, args.out, args.sign, args.keystore,
            args.keyalias, args.keypass, args.ignore_zipalign)
    log('Creating output APK finished in {:.2f} seconds'.format(
            timer() - repack_start_time))
    copy_file_to_out_dir(dex_dir, args.out, 'redex-line-number-map', 'line number map', 'redex-line-number-map')
    copy_file_to_out_dir(dex_dir, args.out, 'redex-line-number-map-v2', 'line number map v2', 'redex-line-number-map-v2')
    copy_file_to_out_dir(dex_dir, args.out, 'stats.txt', 'stats', 'redex-stats.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'filename_mappings.txt', 'src strings map', 'redex-src-strings-map.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'outliner-artifacts.bin', 'outliner artifacts', 'redex-outliner-artifacts.bin')
    copy_file_to_out_dir(dex_dir, args.out, 'method_mapping.txt', 'method id map', 'redex-method-id-map.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'class_mapping.txt', 'class id map', 'redex-class-id-map.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'bytecode_offset_map.txt', 'bytecode offset map', 'redex-bytecode-offset-map.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'coldstart_fields_in_R_classes.txt', 'resources accessed during coldstart', 'redex-tracked-coldstart-resources.txt')
    copy_file_to_out_dir(dex_dir, args.out, 'class_dependencies.txt', 'stats', 'redex-class-dependencies.txt')

    if config_dict.get('proguard_map_output', '') != '':
        merge_proguard_maps(
            config_dict['proguard_map_output'],
            args.input_apk,
            args.out,
            dex_dir,
            args.proguard_map)
    else:
        assert 'RenameClassesPass' not in passes_list and\
                'RenameClassesPassV2' not in passes_list

    remove_temp_dirs()


if __name__ == '__main__':
    keys = {}
    try:
        keystore = join(os.environ['HOME'], '.android', 'debug.keystore')
        if isfile(keystore):
            keys['keystore'] = keystore
            keys['keyalias'] = 'androiddebugkey'
            keys['keypass'] = 'android'
    except:
        pass
    args = arg_parser(**keys).parse_args()
    validate_args(args)
    run_redex(args)
