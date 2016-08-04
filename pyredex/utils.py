# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import atexit
import glob
import shutil
import tempfile

from os.path import join

def abs_glob(directory, pattern='*'):
    """
    Returns all files that match the specified glob inside a directory.
    Returns absolute paths. Does not return files that start with '.'
    """
    for result in glob.glob(join(directory, pattern)):
        yield join(directory, result)


def make_temp_dir(name='', debug=False):
    """ Make a temporary directory which will be automatically deleted """
    directory = tempfile.mkdtemp(name)

    if not debug:  # In debug mode, don't delete the directory
        def remove_directory():
            shutil.rmtree(directory)
        atexit.register(remove_directory)
    return directory
