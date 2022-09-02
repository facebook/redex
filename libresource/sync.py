# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# This script will copy all the resource parsing files from AOSP to redex
# so that we can easily stay up-to-date if these files change in AOSP.
from __future__ import absolute_import, division, print_function, unicode_literals

import os
import shutil
import sys


# These are files that have been re-implemented rather than copied from
# AOSP for simplicity
FB_REIMPLEMENTED = {
    "./cutils/atomic.h",  # ./system/core/include/cutils/atomic.h
    "./cutils/log.h",  # ./system/core/include/log/log.h (cutils is alias)
    "./utils/Atomic.h",  # ./system/core/include/utils/Atomic.h
    "./utils/Log.h",  # ./system/core/include/utils/Log.h
}

# list of (local_path : aosp_path) tuples
FILES = [
    ("./FileMap.cpp", "./system/core/libutils/FileMap.cpp"),
    ("./ResourceTypes.cpp", "./frameworks/base/libs/androidfw/ResourceTypes.cpp"),
    ("./SharedBuffer.cpp", "./system/core/libutils/SharedBuffer.cpp"),
    ("./Static.cpp", "./system/core/libutils/Static.cpp"),
    ("./String16.cpp", "./system/core/libutils/String16.cpp"),
    ("./String8.cpp", "./system/core/libutils/String8.cpp"),
    ("./TypeWrappers.cpp", "./frameworks/base/libs/androidfw/TypeWrappers.cpp"),
    ("./Unicode.cpp", "./system/core/libutils/Unicode.cpp"),
    ("./VectorImpl.cpp", "./system/core/libutils/VectorImpl.cpp"),
    (
        "./android/asset_manager.h",
        "./frameworks/native/include/android/asset_manager.h",
    ),
    (
        "./android/configuration.h",
        "./frameworks/native/include/android/configuration.h",
    ),
    ("./androidfw/Asset.h", "./frameworks/base/include/androidfw/Asset.h"),
    (
        "./androidfw/ByteBucketArray.h",
        "./frameworks/base/include/androidfw/ByteBucketArray.h",
    ),
    (
        "./androidfw/ResourceTypes.h",
        "./frameworks/base/include/androidfw/ResourceTypes.h",
    ),
    (
        "./androidfw/TypeWrappers.h",
        "./frameworks/base/include/androidfw/TypeWrappers.h",
    ),
    ("./system/graphics.h", "./system/core/include/system/graphics.h"),
    ("./system/thread_defs.h", "./system/core/include/system/thread_defs.h"),
    ("./utils/AndroidThreads.h", "./system/core/include/utils/AndroidThreads.h"),
    ("./utils/ByteOrder.h", "./system/core/include/utils/ByteOrder.h"),
    ("./utils/Compat.h", "./system/core/include/utils/Compat.h"),
    ("./utils/Condition.h", "./system/core/include/utils/Condition.h"),
    ("./utils/Debug.h", "./system/core/include/utils/Debug.h"),
    ("./utils/Errors.h", "./system/core/include/utils/Errors.h"),
    ("./utils/FileMap.h", "./system/core/include/utils/FileMap.h"),
    ("./utils/KeyedVector.h", "./system/core/include/utils/KeyedVector.h"),
    ("./utils/Mutex.h", "./system/core/include/utils/Mutex.h"),
    ("./utils/RefBase.h", "./system/core/include/utils/RefBase.h"),
    ("./utils/RWLock.h", "./system/core/include/utils/RWLock.h"),
    ("./utils/SharedBuffer.h", "./system/core/include/utils/SharedBuffer.h"),
    ("./utils/SortedVector.h", "./system/core/include/utils/SortedVector.h"),
    ("./utils/String16.h", "./system/core/include/utils/String16.h"),
    ("./utils/String8.h", "./system/core/include/utils/String8.h"),
    ("./utils/StrongPointer.h", "./system/core/include/utils/StrongPointer.h"),
    ("./utils/Thread.h", "./system/core/include/utils/Thread.h"),
    ("./utils/ThreadDefs.h", "./system/core/include/utils/ThreadDefs.h"),
    ("./utils/threads.h", "./system/core/include/utils/threads.h"),
    ("./utils/Timers.h", "./system/core/include/utils/Timers.h"),
    ("./utils/TypeHelpers.h", "./system/core/include/utils/TypeHelpers.h"),
    ("./utils/Unicode.h", "./system/core/include/utils/Unicode.h"),
    ("./utils/Vector.h", "./system/core/include/utils/Vector.h"),
    ("./utils/VectorImpl.h", "./system/core/include/utils/VectorImpl.h"),
]

if __name__ == "__main__":

    if len(sys.argv) != 2:
        sys.exit("Usage: sync.py <aosp_root_dir>")

    aosp_root = sys.argv[-1]

    # Folder containing the script
    redex_root = os.path.dirname(os.path.realpath(__file__))

    for local_path, aosp_path in FILES:
        if local_path in FB_REIMPLEMENTED:
            sys.exit(
                "Refusing to overwrite our {} with AOSP version".format(local_path)
            )
        aosp_abs_path = os.path.join(aosp_root, aosp_path)
        if not os.path.isfile(aosp_abs_path):
            sys.exit(aosp_abs_path + " not found, aborting.")

        local_abs_path = os.path.join(redex_root, local_path)
        print(aosp_path + " -> " + local_path, end="")
        dirname = os.path.dirname(local_abs_path)
        if not os.path.isdir(dirname):
            os.mkdir(dirname)
        shutil.copy(aosp_abs_path, dirname)
        print("  [done]")
