#!/usr/bin/env python

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

import pyredex.unpacker as unpacker
from pyredex.utils import abs_glob, make_temp_dir, remove_temp_dirs
from pyredex.log import log

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


def run_pass(
        executable_path,
        script_args,
        config_path,
        config_json,
        apk_dir,
        dex_dir,
        dexfiles,
        ):

    if executable_path is None:
        executable_path = shutil.which('redex-all')
        if executable_path is None:
            executable_path = join(dirname(abspath(__file__)), 'redex-all')
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
    if script_args.proguard_config:
        args += ['--proguard-config', script_args.proguard_config]
    if script_args.keep:
        args += ['--seeds', script_args.keep]
    if script_args.proguard_map:
        args += ['-Sproguard_map=' + script_args.proguard_map]

    args += ['--jarpath=' + x for x in script_args.jarpaths]
    args += ['-S' + x for x in script_args.passthru]
    args += ['-J' + x for x in script_args.passthru_json]

    args += dexfiles

    start = timer()

    if script_args.debug:
        print(' '.join(args))
        sys.exit()

    # Our CI system occasionally fails because it is trying to write the
    # redex-all binary when this tries to run.  This shouldn't happen, and
    # might be caused by a JVM bug.  Anyways, let's retry and hope it stops.
    for i in range(5):
        try:
            subprocess.check_call(args)
        except OSError as err:
            if err.errno == errno.ETXTBSY:
                if i < 4:
                    time.sleep(5)
                    continue
            raise err
        break
    log('Dex processing finished in {:.2f} seconds'.format(timer() - start))


def dex_glob(directory):
    """
    Return the dexes in a given directory, with the primary dex first.
    """
    primary = 'classes.dex'
    result = []
    if isfile(join(directory, primary)):
        result.append(join(directory, primary))

    if isfile(join(directory, 'classes2.dex')):
        format = 'classes%d.dex'
        start = 2
    else:
        format = 'secondary-%d.dex'
        start = 1

    for i in range(start, 100):
        dex = join(directory, format % i)
        if not isfile(dex):
            break
        result += [dex]

    return result


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


def zipalign(unaligned_apk_path, output_apk_path):
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
        shutil.copy(unaligned_apk_path, output_apk_path)
    os.remove(unaligned_apk_path)


def create_output_apk(extracted_apk_dir, output_apk_path, sign, keystore,
        key_alias, key_password):

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

    zipalign(unaligned_apk_path, output_apk_path)


def merge_proguard_map_with_rename_output(
        input_apk_path,
        apk_output_path,
        config_dict,
        pg_file):
    log('running merge proguard step')
    redex_rename_map_path = config_dict['RenameClassesPass']['class_rename']
    log('redex map is at ' + str(redex_rename_map_path))
    if os.path.isfile(redex_rename_map_path):
        redex_pg_file = "redex-class-rename-map.txt"
        # find proguard file
        if pg_file:
            output_dir = os.path.dirname(apk_output_path)
            output_file = output_file = join(output_dir, redex_pg_file)
            update_proguard_mapping_file(pg_file, redex_rename_map_path, output_file)
            log('merging proguard map with redex class rename map')
            log('pg mapping file input is ' + str(pg_file))
            log('wrote redex pg format mapping file to ' + str(output_file))
        else:
            log('no proguard map file found')
    else:
        log('Skipping merging of rename maps, since redex rename map file not found')


def update_proguard_mapping_file(pg_map, redex_map, output_file):
    with open(pg_map, 'r') as pg_map, open(redex_map, 'r') as redex_map, open(output_file, 'w') as output:
        redex_dict = {}
        for line in redex_map:
            pair = line.split(' -> ')
            unmangled = pgize(pair[0])
            mangled = pgize(pair[1])
            redex_dict[unmangled] = mangled
        for line in pg_map:
            match_obj = re.match(r'^(.*) -> (.*):', line)
            if match_obj:
                unmangled = match_obj.group(1)
                mangled = match_obj.group(2)
                new_mapping = line.rstrip()
                if unmangled in redex_dict:
                    out_mangled = redex_dict[unmangled]
                    new_mapping = unmangled + ' -> ' + redex_dict[unmangled] + ':'
                    print(new_mapping, file=output)
                else:
                    print(line.rstrip(), file=output)
            else:
                print(line.rstrip(), file=output)

def copy_file_to_out_dir(tmp, apk_output_path, name, human_name, out_name):
    output_dir = os.path.dirname(apk_output_path)
    output_path = os.path.join(output_dir, out_name)
    tmp_path = tmp + '/' + name
    if os.path.isfile(tmp_path):
        subprocess.check_call(['cp', tmp_path, output_path])
        log('Copying ' + human_name + ' map to output dir')
    else:
        log('Skipping ' + human_name + ' copy, since no file found to copy')

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

    parser.add_argument('-P', '--proguard-config', nargs='?',
            help='Path to proguard config')

    parser.add_argument('-k', '--keep', nargs='?',
            help='Path to file containing classes to keep')

    parser.add_argument('-S', dest='passthru', action='append', default=[],
            help='Arguments passed through to redex')
    parser.add_argument('-J', dest='passthru_json', action='append', default=[],
            help='JSON-formatted arguments passed through to redex')

    return parser

def relocate_tmp(d, newtmp):
    """
    Walks through the config dict and changes and string value that begins with
    "/tmp/" to our tmp dir for this run. This is to avoid collisions of
    simultaneously running redexes.
    """
    for k, v in d.items():
        if isinstance(v, dict):
            relocate_tmp(v, newtmp)
        else:
            if (isinstance(v, str) or isinstance(v, unicode)) and v.startswith("/tmp/"):
                d[k] = newtmp + "/" + v[5:]
                log("Replaced {0} in config with {1}".format(v, d[k]))

def run_redex(args):
    debug_mode = args.unpack_only or args.debug

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
    for module in application_modules:
        log('found module: ' + module.get_name() + ' ' + module.get_canary_prefix())
        store_path = os.path.join(dex_dir, module.get_name())
        os.mkdir(store_path)
        module.unpackage(extracted_apk_dir, store_path)
        store_files.append(module.write_redex_metadata(store_path))

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

    config = args.config
    binary = args.redex_binary
    log('Using config ' + (config if config is not None else '(default)'))
    log('Using binary ' + (binary if binary is not None else '(default)'))

    if config is None:
        config_dict = {}
        passes_list = []
    else:
        with open(config) as config_file:
            config_dict = json.load(config_file)
            passes_list = config_dict['redex']['passes']

    newtmp = tempfile.mkdtemp()
    log('Replacing /tmp in config with {}'.format(newtmp))

    # Fix up the config dict to relocate all /tmp references
    relocate_tmp(config_dict, newtmp)

    # Rewrite the relocated config file to our tmp, for use by redex binary
    if config is not None:
        config = newtmp + "/rewritten.config"
        with open(config, 'w') as fp:
            json.dump(config_dict, fp)

    log('Running redex-all on {} dex files '.format(len(dexen)))
    run_pass(binary,
             args,
             config,
             config_dict,
             extracted_apk_dir,
             dex_dir,
             dexen)

    # This file was just here so we could scan it for classnames, but we don't
    # want to pack it back up into the apk
    if os.path.exists(temporary_lib_file):
        os.remove(temporary_lib_file)

    repack_start_time = timer()

    log('Repacking dex files')
    have_locators = config_dict.get("emit_locator_strings")
    dex_mode.repackage(extracted_apk_dir, dex_dir, have_locators)

    for module in application_modules:
        log('repacking module: ' + module.get_name())
        module.repackage(extracted_apk_dir, dex_dir, have_locators)


    log('Creating output apk')
    create_output_apk(extracted_apk_dir, args.out, args.sign, args.keystore,
            args.keyalias, args.keypass)
    log('Creating output APK finished in {:.2f} seconds'.format(
            timer() - repack_start_time))
    copy_file_to_out_dir(newtmp, args.out, 'redex-line-number-map', 'line number map', 'redex-line-number-map')
    copy_file_to_out_dir(newtmp, args.out, 'stats.txt', 'stats', 'redex-stats.txt')
    copy_file_to_out_dir(newtmp, args.out, 'filename_mappings.txt', 'src strings map', 'redex-src-strings-map.txt')
    copy_file_to_out_dir(newtmp, args.out, 'method_mapping.txt', 'method id map', 'redex-method-id-map.txt')

    if 'RenameClassesPass' in passes_list:
        merge_proguard_map_with_rename_output(
            args.input_apk,
            args.out,
            config_dict,
            args.proguard_map)
    else:
        log('Skipping rename map merging, because we didn\'t run the rename pass')

    shutil.rmtree(newtmp)
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
    run_redex(args)
