#!/usr/bin/env python3

import argparse
import atexit
import distutils.version
import functools
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import timeit
import zipfile

from os.path import abspath, basename, dirname, getsize, isdir, isfile, join, \
        realpath, split

timer = timeit.default_timer
uncompressed_extensions = \
        set(['.ogg', '.m4a', '.jpg', '.png', '.arsc', '.xzs'])


def want_trace():
    try:
        trace = os.environ['TRACE']
    except KeyError:
        return False
    for t in trace.split(','):
        try:
            return int(t) > 0
        except ValueError:
            pass
        try:
            (module, level) = t.split(':')
            if module == 'REDEX' and int(level) > 0:
                return True
        except ValueError:
            pass
    return False


def log(*stuff):
    if want_trace():
        print(*stuff, file=sys.stderr)


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

    if executable_path == '':
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
    if script_args.jarpath:
        args += ['--jarpath', script_args.jarpath]
    if script_args.proguard_config:
        args += ['--proguard-config', script_args.proguard_config]
    if script_args.keep:
        args += ['--seeds', script_args.keep]
    if script_args.proguard_map:
        args += ['-Sproguard_map=' + script_args.proguard_map]

    args += ['-S' + x for x in script_args.passthru]
    args += ['-J' + x for x in script_args.passthru_json]

    args += dexfiles

    start = timer()

    args = ' '.join(shlex.quote(x) for x in args)
    if script_args.time:
        args = 'time ' + args

    if script_args.debug:
        print(args)
        sys.exit()

    subprocess.check_call(args, shell=True)
    log('Dex processing finished in {:.2f} seconds'.format(timer() - start))


def abs_glob(directory, pattern='*'):
    """
    Returns all files that match the specified glob inside a directory.
    Returns absolute paths. Does not return files that start with '.'
    """
    for result in glob.glob(join(directory, pattern)):
        yield join(directory, result)


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


def extract_dex_from_jar(jarpath, dexpath):
    dest_directory = dirname(dexpath)
    with zipfile.ZipFile(jarpath) as jar:
        contents = jar.namelist()
        dexfiles = [name for name in contents if name.endswith('dex')]
        assert len(dexfiles) == 1, 'Expected a single dex file'
        dexname = jar.extract(dexfiles[0], dest_directory)
        os.rename(join(dest_directory, dexname), dexpath)


def create_dex_jar(jarpath, dexpath, compression=zipfile.ZIP_STORED):
    with zipfile.ZipFile(jarpath, mode='w') as zf:
        zf.write(dexpath, 'classes.dex', compress_type=compression)
        zf.writestr('/META-INF/MANIFEST.MF',
                b'Manifest-Version: 1.0\n'
                b'Dex-Location: classes.dex\n'
                b'Created-By: redex\n\n')


def make_temp_dir(name='', debug=False):
    """ Make a temporary directory which will be automatically deleted """
    directory = tempfile.mkdtemp(name)

    if not debug:  # In debug mode, don't delete the directory
        def remove_directory():
            shutil.rmtree(directory)
        atexit.register(remove_directory)
    return directory


def extract_apk(apk, destination_directory):
    with zipfile.ZipFile(apk) as z:
        try:
            if z.getinfo('resources.arsc').compress_type == zipfile.ZIP_DEFLATED:
                uncompressed_extensions.remove('.arsc')
        except KeyError:
            log('Unable to determine compression status of resources.arsc')

        log('resources.arsc will be ' +
                ('uncompressed' if '.arsc' in uncompressed_extensions
                    else 'compressed') + ' in output apk')

        z.extractall(destination_directory)


class Api21DexMode(object):
    """
    On API 21+, secondary dex files are in the root of the apk and are named
    classesN.dex for N in [2, 3, 4, ... ]

    Note that this mode will also be used for apps that don't have any
    secondary dex files.
    """

    _secondary_dir = 'assets/secondary-program-dex-jars'

    def detect(self, extracted_apk_dir):
        # Note: This mode is the fallback and we only check for it after
        # checking for the other modes. This should return true for any
        # apk.
        return isfile(join(extracted_apk_dir, 'classes.dex'))

    def unpackage(self, extracted_apk_dir, dex_dir):
        jar_meta_path = join(extracted_apk_dir, self._secondary_dir,
                             'metadata.txt')
        if os.path.exists(jar_meta_path):
            os.remove(jar_meta_path)
        for path in abs_glob(extracted_apk_dir, '*.dex'):
            shutil.move(path, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators):
        shutil.move(join(dex_dir, 'classes.dex'), extracted_apk_dir)

        if not os.path.exists(join(extracted_apk_dir,
                              'assets', 'secondary-program-dex-jars')):
            return
        jar_meta_path = join(extracted_apk_dir,
                             self._secondary_dir,
                             'metadata.txt')
        with open(jar_meta_path, 'w') as jar_meta:
            jar_meta.write('.root_relative\n')
            if have_locators:
                jar_meta.write('.locators\n')
            for i in range(2, 100):
                dex_path = join(dex_dir, 'classes%d.dex' % i)
                if not isfile(dex_path):
                    break
                with open(dex_path, 'rb') as dex:
                    sha1hash = hashlib.sha1(dex.read()).hexdigest()
                jar_meta.write(
                    'classes%d.dex %s secondary.dex%02d.Canary\n'
                    % (i, sha1hash, i - 1))
                shutil.move(dex_path, extracted_apk_dir)

class SubdirDexMode(object):
    """
    `buck build katana` places secondary dexes in a subdir with no compression
    """

    _secondary_dir = 'assets/secondary-program-dex-jars'

    def detect(self, extracted_apk_dir):
        secondary_dex_dir = join(extracted_apk_dir, self._secondary_dir)
        return isdir(secondary_dex_dir) and \
                len(list(abs_glob(secondary_dex_dir, '*.dex.jar')))

    def unpackage(self, extracted_apk_dir, dex_dir):
        jars = abs_glob(join(extracted_apk_dir, self._secondary_dir),
                        '*.dex.jar')
        for jar in jars:
            dexpath = join(dex_dir, basename(jar))[:-4]
            extract_dex_from_jar(jar, dexpath)
            os.remove(jar + ".meta")
            os.remove(jar)
        os.remove(join(extracted_apk_dir, self._secondary_dir, 'metadata.txt'))
        shutil.move(join(extracted_apk_dir, 'classes.dex'), dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators):
        shutil.move(join(dex_dir, 'classes.dex'), extracted_apk_dir)

        jar_meta_path = join(dex_dir, 'metadata.txt')
        with open(jar_meta_path, 'w') as jar_meta:
            if have_locators:
                jar_meta.write('.locators\n')
            for i in range(1, 100):
                oldpath = join(dex_dir, 'classes%d.dex' % (i + 1))
                dexpath = join(dex_dir, 'secondary-%d.dex' % i)
                if not isfile(oldpath):
                    break
                shutil.move(oldpath, dexpath)
                jarpath = dexpath + '.jar'
                create_dex_jar(jarpath, dexpath)
                dex_meta_base = 'secondary-%d.dex.jar.meta' % i
                dex_meta_path = join(dex_dir, dex_meta_base)
                with open(dex_meta_path, 'w') as dex_meta:
                    dex_meta.write('jar:%d dex:%d\n' %
                                   (getsize(jarpath), getsize(dexpath)))
                with open(jarpath, 'rb') as jar:
                    sha1hash = hashlib.sha1(jar.read()).hexdigest()
                shutil.move(dex_meta_path,
                            join(extracted_apk_dir, self._secondary_dir))
                shutil.move(jarpath, join(extracted_apk_dir,
                                          self._secondary_dir))
                jar_meta.write(
                    'secondary-%d.dex.jar %s secondary.dex%02d.Canary\n'
                    % (i, sha1hash, i))
        shutil.move(jar_meta_path, join(extracted_apk_dir, self._secondary_dir))

class XZSDexMode(object):
    """
    Secondary dex files are packaged in individual jar files where are then
    concatenated together and compressed with xz.

    ... This format is completely insane.
    """
    _xzs_dir = 'assets/secondary-program-dex-jars'
    _xzs_filename = 'secondary.dex.jar.xzs'

    def detect(self, extracted_apk_dir):
        path = join(extracted_apk_dir, self._xzs_dir,
                self._xzs_filename)
        return isfile(path)

    def unpackage(self, extracted_apk_dir, dex_dir):
        src = join(extracted_apk_dir, self._xzs_dir,
                self._xzs_filename)
        dest = join(dex_dir, self._xzs_filename)

        # Move secondary dexen
        shutil.move(src, dest)

        # concat_jar is a bunch of .dex.jar files concatenated together.
        concat_jar = join(dex_dir, 'secondary.dex.jar')
        cmd = 'cat {} | xz -d --threads 6 > {}'.format(dest, concat_jar)
        subprocess.check_call(cmd, shell=True)

        # Sizes of the concatenated .dex.jar files are stored in .meta files.
        # Read the sizes of each .dex.jar file and un-concatenate them.
        jar_size_regex = 'jar:(\d+)'
        secondary_dir = join(extracted_apk_dir, self._xzs_dir)
        jar_sizes = {}
        for i in range(1, 100):
            filename = 'secondary-' + str(i) + '.dex.jar.xzs.tmp~.meta'
            metadata_path = join(secondary_dir, filename)
            if isfile(metadata_path):
                with open(metadata_path) as f:
                    jar_sizes[i] = \
                            int(re.match(jar_size_regex, f.read()).group(1))
                os.remove(metadata_path)
            else:
                break

        with open(concat_jar, 'rb') as cj:
            for i in range(1, len(jar_sizes) + 1):
                jarpath = join(dex_dir, 'secondary-%d.dex.jar' % i)
                with open(jarpath, 'wb') as jar:
                    jar.write(cj.read(jar_sizes[i]))

        for j in jar_sizes.keys():
            assert jar_sizes[j] == getsize(dex_dir + '/secondary-' + str(j) + '.dex.jar')

        assert sum(jar_sizes.values()) == getsize(concat_jar)

        # Clean up everything other than dexen in the dex directory
        os.remove(concat_jar)
        os.remove(dest)

        # Lastly, unzip all the jar files and delete them
        for jarpath in abs_glob(dex_dir, '*.jar'):
            extract_dex_from_jar(jarpath, jarpath[:-4])
            os.remove(jarpath)

        # Move primary dex
        shutil.move(join(extracted_apk_dir, 'classes.dex'), dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators):
        # Move primary dex
        shutil.move(join(dex_dir, 'classes.dex'), extracted_apk_dir)

        dex_sizes = {}
        jar_sizes = {}

        # Package each dex into a jar
        for i in range(1, 100):
            oldpath = join(dex_dir, 'classes%d.dex' % (i + 1))
            if not isfile(oldpath):
                break
            dexpath = join(dex_dir, 'secondary-%d.dex' % i)
            shutil.move(oldpath, dexpath)
            jarpath = dexpath + '.jar'
            create_dex_jar(jarpath, dexpath)
            dex_sizes[jarpath] = getsize(dexpath)
            jar_sizes[jarpath] = getsize(jarpath)

        concat_jar_path = join(dex_dir, 'secondary.dex.jar')
        concat_jar_meta = join(dex_dir, 'metadata.txt')

        # Concatenate the jar files and create corresponding metadata files
        with open(concat_jar_path, 'wb') as concat_jar:
            with open(concat_jar_meta, 'w') as concat_meta:
                if have_locators:
                    concat_meta.write('.locators\n')

                for i in range(1, 100):
                    jarpath = join(dex_dir, 'secondary-%d.dex.jar' % i)
                    if not isfile(jarpath):
                        break

                    with open(jarpath + '.xzs.tmp~.meta', 'wb') as metadata:
                        sizes = 'jar:{} dex:{}'.format(
                            jar_sizes[jarpath], dex_sizes[jarpath])
                        metadata.write(bytes(sizes, 'ascii'))

                    with open(jarpath, 'rb') as jar:
                        contents = jar.read()
                        concat_jar.write(contents)
                        sha1hash = hashlib.sha1(contents).hexdigest()

                    concat_meta.write(
                        '%s.xzs.tmp~ %s secondary.dex%02d.Canary\n'
                        % (basename(jarpath), sha1hash, i))

        assert getsize(concat_jar_path) == sum(getsize(x)
                for x in abs_glob(dex_dir, 'secondary-*.dex.jar'))

        # XZ-compress the result
        subprocess.check_call(['xz', '-z6', '--check=crc32', '--threads=6',
                concat_jar_path])

        # Copy all the archive and metadata back to the apk directory
        secondary_dex_dir = join(extracted_apk_dir, self._xzs_dir)
        for path in abs_glob(dex_dir, '*.meta'):
            shutil.copy(path, secondary_dex_dir)
        shutil.copy(concat_jar_meta, join(secondary_dex_dir, 'metadata.txt'))
        shutil.copy(concat_jar_path + '.xz',
                join(secondary_dex_dir, self._xzs_filename))


# These are checked in order from top to bottom. The first one to have detect()
# return true will be used.
SECONDARY_DEX_MODES = [
    XZSDexMode(),
    SubdirDexMode(),
    Api21DexMode(),
]


def detect_secondary_dex_mode(extracted_apk_dir):
    for mode in SECONDARY_DEX_MODES:
        if mode.detect(extracted_apk_dir):
            return mode
    sys.exit('Unknown secondary dex mode')


def should_compress(filename):
    return not any(filename.endswith(ext) for ext in uncompressed_extensions)


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
                compress = zipfile.ZIP_DEFLATED if should_compress(filename) \
                        else zipfile.ZIP_STORED
                filepath = join(dirpath, filename)
                archivepath = filepath[len(extracted_apk_dir):]
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


def copy_stats_to_out_dir(tmp, apk_output_path):
    output_dir = os.path.dirname(apk_output_path)
    output_stats_path = os.path.join(output_dir, "redex-stats.txt")
    if os.path.isfile(tmp + '/stats.txt'):
        subprocess.check_call(['cp', tmp + '/stats.txt', output_stats_path])
        log('Copying stats to output dir')
    else:
        log('Skipping stats copy, since no file found to copy')

def copy_filename_map_to_out_dir(tmp, apk_output_path):
    output_dir = os.path.dirname(apk_output_path)
    output_filemap_path = os.path.join(output_dir, "redex-src-strings-map.txt")
    if os.path.isfile(tmp + '/filename_mappings.txt'):
        subprocess.check_call(['cp', tmp + '/filename_mappings.txt', output_filemap_path])
        log('Copying src string map to output dir')
    else:
        log('Skipping src string map copy, since no file found to copy')

    output_methodmap_path = os.path.join(output_dir, "redex-method-id-map.txt")
    if os.path.isfile(tmp + '/method_mapping.txt'):
        subprocess.check_call(['cp', tmp + '/method_mapping.txt', output_methodmap_path])
        log('Copying method id map to output dir')
    else:
        log('Skipping method id map copy, since no file found to copy')


def arg_parser(config=None, keystore=None, keyalias=None, keypass=None):
    description = """
Given an APK, produce a better APK!

"""
    parser = argparse.ArgumentParser(
            formatter_class=argparse.RawDescriptionHelpFormatter,
            description=description)

    parser.add_argument('input_apk', help='Input APK file')
    parser.add_argument('-o', '--out', nargs='?', default='redex-out.apk',
            help='Output APK file name (defaults to redex-out.apk)')
    parser.add_argument('-j', '--jarpath', nargs='?')

    parser.add_argument('redex_binary', nargs='?', default='',
            help='Path to redex binary')

    parser.add_argument('-c', '--config', default=config,
            help='Configuration file')

    parser.add_argument('-t', '--time', action='store_true',
            help='Run redex passes with `time` to print CPU and wall time')

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
            if isinstance(v, str) and v.startswith("/tmp/"):
                d[k] = newtmp + "/" + v[5:]
                log("Replaced {0} in config with {1}".format(v, d[k]))

def run_redex(args):
    debug_mode = args.unpack_only or args.debug

    unpack_start_time = timer()
    extracted_apk_dir = make_temp_dir('.redex_extracted_apk', debug_mode)

    log('Extracting apk...')
    extract_apk(args.input_apk, extracted_apk_dir)

    dex_mode = detect_secondary_dex_mode(extracted_apk_dir)
    log('Detected dex mode ' + str(type(dex_mode).__name__))
    dex_dir = make_temp_dir('.redex_dexen', debug_mode)

    log('Unpacking dex files')
    dex_mode.unpackage(extracted_apk_dir, dex_dir)

    # Some of the native libraries can be concatenated together into one
    # xz-compressed file. We need to decompress that file so that we can scan
    # through it looking for classnames.
    xz_compressed_libs = join(extracted_apk_dir, 'assets/lib/libs.xzs')
    temporary_lib_file = join(extracted_apk_dir, 'lib/concated_native_libs.so')
    if os.path.exists(xz_compressed_libs):
        cmd = 'xz -d --stdout {} > {}'.format(
                shlex.quote(xz_compressed_libs),
                shlex.quote(temporary_lib_file))
        subprocess.check_call(cmd, shell=True)

    if args.unpack_only:
        print('APK: ' + extracted_apk_dir)
        print('DEX: ' + dex_dir)
        sys.exit()

    # Move each dex to a separate temporary directory to be operated by
    # redex.
    dexen = move_dexen_to_directories(dex_dir, dex_glob(dex_dir))
    log('Unpacking APK finished in {:.2f} seconds'.format(
            timer() - unpack_start_time))

    config = args.config
    binary = args.redex_binary
    log('Using config ' + (config if config is not None else '(default)'))
    log('Using binary ' + binary)

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

    log('Creating output apk')
    create_output_apk(extracted_apk_dir, args.out, args.sign, args.keystore,
            args.keyalias, args.keypass)
    log('Creating output APK finished in {:.2f} seconds'.format(
            timer() - repack_start_time))
    copy_stats_to_out_dir(newtmp, args.out)
    copy_filename_map_to_out_dir(newtmp, args.out)

    if 'RenameClassesPass' in passes_list:
        merge_proguard_map_with_rename_output(
            args.input_apk,
            args.out,
            config_dict,
            args.proguard_map)
    else:
        log('Skipping rename map merging, because we didn\'t run the rename pass')

    shutil.rmtree(newtmp)


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
