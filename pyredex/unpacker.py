# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import hashlib
import json
import os
import re
import subprocess
import shutil
import string
import zipfile

from os.path import basename, dirname, getsize, isdir, isfile, join

from pyredex.utils import abs_glob, make_temp_dir
from pyredex.logger import log

class ApplicationModule(object):

    def __init__(self, extracted_apk_dir, name, canary_prefix, dependencies):
        self.name = name
        self.path = join('assets', name)
        self.canary_prefix = canary_prefix
        self.dependencies = dependencies

    @staticmethod
    def detect(extracted_apk_dir):
        modules = []
        for candidate in abs_glob(extracted_apk_dir, 'assets/*/metadata.txt'):
            with open(candidate) as metadata:
                name = None
                dependencies = []
                canary_match = None
                canary_prefix = None
                for line in metadata.read().splitlines():
                    tokens = line.split()
                    if tokens[0] == '.id':
                        name = tokens[1]
                    if tokens[0] == '.requires':
                        dependencies.append(tokens[1])
                    if tokens[0][0] != '.':
                        canary_match = re.search('([A-Za-z0-9]*)[.]dex[0-9]+[.]Canary', tokens[2])
                        if canary_match is not None:
                            canary_prefix = canary_match.group(1)
                if name is not None:
                    modules.append(ApplicationModule(extracted_apk_dir, name, canary_prefix, dependencies))
        return modules

    def get_name(self):
        return self.name

    def get_canary_prefix(self):
        return self.canary_prefix

    def write_redex_metadata(self, path, metadata_file):
        files = []
        for x in abs_glob(path, '*.dex'):
            files.append(x)
        metadata = {'id': self.name,
                    'requires': self.dependencies,
                    'files': files}
        with open(metadata_file, 'w') as store_metadata:
            json.dump(metadata, store_metadata)

    def unpackage(self, extracted_apk_dir, dex_dir):
        self.dex_mode = XZSDexMode(dex_asset_dir=self.path,
                                   store_name=self.name,
                                   dex_prefix=self.name,
                                   canary_prefix=self.canary_prefix,
                                   store_id=self.name,
                                   dependencies=self.dependencies)
        if (self.dex_mode.detect(extracted_apk_dir)):
            log('module ' + self.name + ' is XZSDexMode')
            self.dex_mode.unpackage(extracted_apk_dir, dex_dir)
        else:
            self.dex_mode = SubdirDexMode(dex_asset_dir=self.path,
                                          store_name=self.name,
                                          dex_prefix=self.name,
                                          canary_prefix=self.canary_prefix,
                                          store_id=self.name,
                                          dependencies=self.dependencies)
            if (self.dex_mode.detect(extracted_apk_dir)):
                log('module ' + self.name + ' is SubdirDexMode')
                self.dex_mode.unpackage(extracted_apk_dir, dex_dir)
            else:
                self.dex_mode = Api21ModuleDexMode(dex_asset_dir=self.path,
                                                   store_name=self.name,
                                                   canary_prefix=self.canary_prefix,
                                                   store_id=self.name,
                                                   dependencies=self.dependencies)
                log('module ' + self.name + ' is Api21ModuleDexMode')
                self.dex_mode.unpackage(extracted_apk_dir, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators, locator_store_id):
        self.dex_mode.repackage(extracted_apk_dir, dex_dir, have_locators, locator_store_id)

class DexMetadata(object):
    def __init__(self,
                 store=None,
                 dependencies=None,
                 have_locators=False,
                 is_root_relative=False,
                 locator_store_id=0):
        self._have_locators = False
        self._store = store
        self._dependencies = dependencies
        self._have_locators = have_locators
        self._is_root_relative = is_root_relative
        self._dexen = []
        self._locator_store_id = locator_store_id

    def add_dex(self, dex_path, canary_class, hash=None):
        if hash is None:
            with open(dex_path, 'rb') as dex:
                sha1hash = hashlib.sha1(dex.read()).hexdigest()
        else:
            sha1hash = hash
        self._dexen.append(
            (os.path.basename(dex_path), sha1hash, canary_class))

    def write(self, path):
        with open(path, 'w') as meta:
            if self._store is not None:
                meta.write('.id ' + self._store + '\n')
            if self._dependencies is not None:
                for dependency in self._dependencies:
                    meta.write('.requires ' + dependency + '\n')
            if self._is_root_relative:
                meta.write('.root_relative\n')
            if self._have_locators:
                meta.write('.locators\n')
            if self._locator_store_id > 0:
                meta.write('.locator_id ' + str(self._locator_store_id) + '\n')
            for dex in self._dexen:
                meta.write(' '.join(dex) + '\n')


class BaseDexMode(object):
    def __init__(self, dex_prefix, canary_prefix, store_id, dependencies):
        self._dex_prefix = dex_prefix
        self._canary_prefix = canary_prefix
        self._store_id = store_id
        self._dependencies = dependencies

    def unpackage(self, extracted_apk_dir, dex_dir):
        primary_dex = join(extracted_apk_dir, self._dex_prefix + '.dex')
        if os.path.exists(primary_dex):
            shutil.move(primary_dex, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators):
        primary_dex = join(dex_dir, self._dex_prefix + '.dex')
        if os.path.exists(primary_dex):
            shutil.move(primary_dex, extracted_apk_dir)

    def get_canary(self, i):
        return self._canary_prefix + '.dex%02d.Canary' % i

class Api21DexMode(BaseDexMode):
    """
    On API 21+, secondary dex files are in the root of the apk and are named
    classesN.dex for N in [2, 3, 4, ... ]

    Note that this mode will also be used for apps that don't have any
    secondary dex files.
    """

    def __init__(self,
                 dex_asset_dir='assets/secondary-program-dex-jars',
                 dex_prefix='classes',
                 canary_prefix='secondary',
                 is_root_relative=True,
                 store_id=None, dependencies=None):
        BaseDexMode.__init__(self, dex_prefix, canary_prefix, store_id, dependencies)
        self._secondary_dir = dex_asset_dir
        self._is_root_relative = is_root_relative

    def detect(self, extracted_apk_dir):
        # Note: This mode is the fallback and we only check for it after
        # checking for the other modes. This should return true for any
        # apk.
        return isfile(join(extracted_apk_dir, self._dex_prefix + '.dex'))

    def unpackage(self, extracted_apk_dir, dex_dir):
        BaseDexMode.unpackage(self, extracted_apk_dir, dex_dir)

        metadata_dir = join(extracted_apk_dir, self._secondary_dir)
        if self._is_root_relative:
            extracted_dex_dir = extracted_apk_dir
        else:
            extracted_dex_dir = metadata_dir
        for path in abs_glob(extracted_dex_dir, '*.dex'):
            shutil.move(path, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators, locator_store_id=0):
        BaseDexMode.repackage(self, extracted_apk_dir, dex_dir, have_locators)
        metadata_dir = join(extracted_apk_dir, self._secondary_dir)

        metadata = DexMetadata(is_root_relative=self._is_root_relative,
                               have_locators=have_locators,
                               store=self._store_id,
                               dependencies=self._dependencies,
                               locator_store_id=locator_store_id)
        for i in range(2, 100):
            dex_path = join(dex_dir, self._dex_prefix + '%d.dex' % i)
            if not isfile(dex_path):
                break
            metadata.add_dex(dex_path, BaseDexMode.get_canary(self, i - 1))
            if self._is_root_relative:
                shutil.move(dex_path, extracted_apk_dir)
            else:
                shutil.move(dex_path, metadata_dir)
        if os.path.exists(metadata_dir):
            metadata.write(join(metadata_dir, 'metadata.txt'))

class Api21ModuleDexMode(Api21DexMode):
    """
    modules built in Api21 builds will just have <store_name><i>.dex files in
    the module directory.  This should only be used by modules.
    """

    def __init__(self,
                 dex_asset_dir,
                 store_name='secondary',
                 canary_prefix='secondary',
                 store_id=None,
                 dependencies=None):
        Api21DexMode.__init__(self,
                              dex_asset_dir=dex_asset_dir,
                              dex_prefix=store_name,
                              canary_prefix=canary_prefix,
                              store_id=store_id,
                              dependencies=dependencies,
                              is_root_relative=False)
        self._secondary_dir = dex_asset_dir
        self._store_name = store_name

    def detect(self, extracted_apk_dir):
        secondary_dex_dir = join(extracted_apk_dir, self._secondary_dir)
        return len(list(abs_glob(secondary_dex_dir, '*.dex')))

class SubdirDexMode(BaseDexMode):
    """
    `buck build katana` places secondary dexes in a subdir with no compression
    """

    def __init__(self,
                 dex_asset_dir='assets/secondary-program-dex-jars',
                 store_name='secondary',
                 dex_prefix='classes',
                 canary_prefix='secondary',
                 store_id=None,
                 dependencies=None):
        BaseDexMode.__init__(self, dex_prefix, canary_prefix, store_id, dependencies)
        self._secondary_dir = dex_asset_dir
        self._store_name = store_name

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
            os.remove(jar + '.meta')
            os.remove(jar)
        os.remove(join(extracted_apk_dir, self._secondary_dir, 'metadata.txt'))
        BaseDexMode.unpackage(self, extracted_apk_dir, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators, locator_store_id=0):
        BaseDexMode.repackage(self, extracted_apk_dir, dex_dir, have_locators)

        metadata = DexMetadata(have_locators=have_locators,
                               store=self._store_id,
                               dependencies=self._dependencies,
                               locator_store_id=locator_store_id)
        for i in range(1, 100):
            oldpath = join(dex_dir, self._dex_prefix + '%d.dex' % (i + 1))
            dexpath = join(dex_dir, self._store_name + '-%d.dex' % i)
            if not isfile(oldpath):
                break
            shutil.move(oldpath, dexpath)

            jarpath = dexpath + '.jar'
            create_dex_jar(jarpath, dexpath)
            metadata.add_dex(jarpath, BaseDexMode.get_canary(self, i))

            dex_meta_base = jarpath + '.meta'
            dex_meta_path = join(dex_dir, dex_meta_base)
            with open(dex_meta_path, 'w') as dex_meta:
                dex_meta.write('jar:%d dex:%d\n' %
                               (getsize(jarpath), getsize(dexpath)))

            shutil.move(dex_meta_path,
                        join(extracted_apk_dir, self._secondary_dir))
            shutil.move(jarpath, join(extracted_apk_dir,
                                      self._secondary_dir))
        jar_meta_path = join(dex_dir, 'metadata.txt')
        metadata.write(jar_meta_path)
        shutil.move(jar_meta_path, join(extracted_apk_dir, self._secondary_dir))

class XZSDexMode(BaseDexMode):
    """
    Secondary dex files are packaged in individual jar files where are then
    concatenated together and compressed with xz.

    ... This format is completely insane.
    """

    def __init__(self,
                 dex_asset_dir='assets/secondary-program-dex-jars',
                 store_name='secondary',
                 dex_prefix='classes',
                 canary_prefix='secondary',
                 store_id=None,
                 dependencies=None):
        BaseDexMode.__init__(self, dex_prefix, canary_prefix, store_id, dependencies)
        self._xzs_dir = dex_asset_dir
        self._xzs_filename = store_name + '.dex.jar.xzs'
        self._store_name = store_name

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
        concat_jar = join(dex_dir, self._xzs_filename[:-4])
        cmd = 'cat {} | xz -d --threads 6 > {}'.format(dest, concat_jar)
        subprocess.check_call(cmd, shell=True)

        dex_order = []
        with open(join(extracted_apk_dir, self._xzs_dir, 'metadata.txt')) as dex_metadata:
            for line in dex_metadata.read().splitlines():
                if line[0] != '.':
                    tokens = line.split()
                    search_pattern = self._store_name + '-(\d+)\.dex\.jar\.xzs\.tmp~'
                    match = re.search(search_pattern, tokens[0])
                    if match is None:
                        raise Exception('unable to find match in ' + tokens[0] + ' for ' + search_pattern)
                    dex_order.append(int(match.group(1)))

        # Sizes of the concatenated .dex.jar files are stored in .meta files.
        # Read the sizes of each .dex.jar file and un-concatenate them.
        jar_size_regex = 'jar:(\d+)'
        secondary_dir = join(extracted_apk_dir, self._xzs_dir)
        jar_sizes = {}
        for i in dex_order:
            filename = self._store_name + '-%d.dex.jar.xzs.tmp~.meta' % i
            metadata_path = join(secondary_dir, filename)
            if isfile(metadata_path):
                with open(metadata_path) as f:
                    jar_sizes[i] = \
                            int(re.match(jar_size_regex, f.read()).group(1))
                os.remove(metadata_path)
                log('found jar ' + filename + ' of size ' + str(jar_sizes[i]))
            else:
                break

        with open(concat_jar, 'rb') as cj:
            for i in dex_order:
                jarpath = join(dex_dir, self._store_name + '-%d.dex.jar' % i)
                with open(jarpath, 'wb') as jar:
                    jar.write(cj.read(jar_sizes[i]))

        for j in jar_sizes.keys():
            jar_size = getsize(dex_dir + '/' + self._store_name + '-' + str(j) + '.dex.jar')
            log('validating ' + self._store_name + '-' + str(j) + '.dex.jar size=' + str(jar_size) + ' expecting=' + str(jar_sizes[j]))
            assert jar_sizes[j] == jar_size

        assert sum(jar_sizes.values()) == getsize(concat_jar)

        # Clean up everything other than dexen in the dex directory
        os.remove(concat_jar)
        os.remove(dest)

        # Lastly, unzip all the jar files and delete them
        for jarpath in abs_glob(dex_dir, '*.jar'):
            extract_dex_from_jar(jarpath, jarpath[:-4])
            os.remove(jarpath)
        BaseDexMode.unpackage(self, extracted_apk_dir, dex_dir)

    def repackage(self, extracted_apk_dir, dex_dir, have_locators, locator_store_id=0):
        BaseDexMode.repackage(self, extracted_apk_dir, dex_dir, have_locators)

        dex_sizes = {}
        jar_sizes = {}

        concat_jar_path = join(dex_dir, self._store_name + '.dex.jar')
        concat_jar_meta = join(dex_dir, 'metadata.txt')
        dex_metadata = DexMetadata(have_locators=have_locators,
                                   store=self._store_id,
                                   dependencies=self._dependencies,
                                   locator_store_id=locator_store_id)

        with open(concat_jar_path, 'wb') as concat_jar:
            for i in range(1, 100):
                oldpath = join(dex_dir, self._dex_prefix + '%d.dex' % (i + 1))
                if not isfile(oldpath):
                    break
                dexpath = join(dex_dir, self._store_name + '-%d.dex' % i)

                # Package each dex into a jar
                shutil.move(oldpath, dexpath)
                jarpath = dexpath + '.jar'
                create_dex_jar(jarpath, dexpath)
                dex_sizes[jarpath] = getsize(dexpath)
                jar_sizes[jarpath] = getsize(jarpath)

                # Concatenate the jar files and create corresponding metadata files
                with open(jarpath + '.xzs.tmp~.meta', 'w') as metadata:
                    sizes = 'jar:{} dex:{}'.format(
                        jar_sizes[jarpath], dex_sizes[jarpath])
                    metadata.write(sizes)

                with open(jarpath, 'rb') as jar:
                    contents = jar.read()
                    concat_jar.write(contents)
                    sha1hash = hashlib.sha1(contents).hexdigest()

                dex_metadata.add_dex(jarpath + '.xzs.tmp~',
                                     BaseDexMode.get_canary(self, i),
                                     hash=sha1hash)

        dex_metadata.write(concat_jar_meta)
        assert getsize(concat_jar_path) == sum(getsize(x)
                for x in abs_glob(dex_dir, self._store_name + '-*.dex.jar'))

        # XZ-compress the result
        subprocess.check_call(['xz', '-z9', '--check=crc32', '--threads=6',
                concat_jar_path])

        # Copy all the archive and metadata back to the apk directory
        secondary_dex_dir = join(extracted_apk_dir, self._xzs_dir)
        for path in abs_glob(dex_dir, self._store_name + '*.meta'):
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
    raise Exception('Unknown secondary dex mode')


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
