# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function

import logging
import mmap
import struct
from collections import namedtuple


Position = namedtuple("Position", "method file line")
MapEntry = namedtuple("MapEntry", "class_id method_id file_id line parent")


class PositionMap(object):
    def __init__(self):
        self.string_pool = []
        self.positions = []

    @staticmethod
    def read_from(filename):
        with open(filename) as f:
            mapping = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            magic = struct.unpack("<L", mapping.read(4))[0]
            if magic != 0xFACEB000:
                raise Exception("Magic number mismatch")
            version = struct.unpack("<L", mapping.read(4))[0]
            if version not in [1, 2]:
                raise Exception("Version mismatch")
            spool_count = struct.unpack("<L", mapping.read(4))[0]
            pmap = PositionMap()
            for _ in range(0, spool_count):
                ssize = struct.unpack("<L", mapping.read(4))[0]
                pmap.string_pool.append(mapping.read(ssize).decode("utf-8"))
            logging.info("Unpacked %d strings from line map", spool_count)
            pos_count = struct.unpack("<L", mapping.read(4))[0]
            # this is pretty slow; it would be much faster in C++ with memcpy
            # and arrays of structs, but I don't know how to call C/C++ from
            # Python under our build system
            for _ in range(0, pos_count):
                if version == 1:
                    pmap.positions.append(
                        MapEntry._make(
                            (None, None) + struct.unpack("<LLL", mapping.read(12))
                        )
                    )
                else:
                    pmap.positions.append(
                        MapEntry._make(struct.unpack("<LLLLL", mapping.read(20)))
                    )
            logging.info("Unpacked %d map entries from line map", pos_count)
            return pmap

    def get_stack(self, idx):
        stack = []
        while idx >= 0 and idx < len(self.positions):
            pi = self.positions[idx]
            if pi.class_id is not None:
                method = (
                    self.string_pool[pi.class_id] + "." + self.string_pool[pi.method_id]
                )
            else:
                method = None
            stack.append(Position(method, self.string_pool[pi.file_id], pi.line))
            idx = pi.parent - 1
        return stack
