#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import logging
import struct
import sys


class IODIMetadata(object):
    # This needs to match the definitions in DexOutput.h!
    IODI_LAYER_BITS = 4
    IODI_LAYER_SHIFT = 32 - IODI_LAYER_BITS
    IODI_DATA_MASK = (1 << IODI_LAYER_SHIFT) - 1
    IODI_LAYER_MASK = ((1 << IODI_LAYER_BITS) - 1) << IODI_LAYER_SHIFT

    def __init__(self, path):
        with open(path, "rb") as f:
            magic, version, count, zero = struct.unpack("<LLLL", f.read(4 * 4))
            if magic != 0xFACEB001:
                raise Exception("Unexpected magic: " + hex(magic))
            if version != 1:
                raise Exception("Unexpected version: " + str(version))
            if zero != 0:
                raise Exception("Unexpected zero: " + str(zero))
            self._entries = {}
            for _ in range(count):
                klen, method_id = struct.unpack("<HQ", f.read(2 + 8))
                form = "<" + str(klen) + "s"
                key = struct.unpack(form, f.read(klen))[0].decode("ascii")
                self._entries[key] = method_id

    def map_iodi(self, debug_line_map, class_name, method_name, input_lineno):
        input_lineno = int(input_lineno)
        qualified_name = class_name + "." + method_name
        layer = (
            input_lineno & IODIMetadata.IODI_LAYER_MASK
        ) >> IODIMetadata.IODI_LAYER_SHIFT
        logging.debug("IODI layer %d", layer)
        adjusted_lineno = input_lineno
        if layer > 0:
            qualified_name += "@" + str(layer)
            adjusted_lineno = input_lineno & IODIMetadata.IODI_DATA_MASK
        logging.debug("IODI adjusted line no %d", layer)
        res_lineno = None if input_lineno == adjusted_lineno else adjusted_lineno

        if qualified_name in self._entries:
            logging.debug("Found %s in entries", qualified_name)
            method_id = self._entries[qualified_name]
            mapped = debug_line_map.find_line_number(method_id, adjusted_lineno)
            if mapped is not None:
                return (mapped, method_id)
            return (res_lineno, method_id)

        return (res_lineno, None)

    def map_iodi_no_debug_to_mappings(self, debug_line_map, class_name, method_name):
        qualified_name = class_name + "." + method_name
        if qualified_name in self._entries:
            method_id = self._entries[qualified_name]
            return debug_line_map.get_mappings(method_id)
        return None

    def _write(self, form, *vals):
        self._f.write(struct.pack(form, *vals))

    def write(self, path):
        with open(path, "wb") as f:
            self._f = f
            self._write("<LLLL", 0xFACEB001, 1, len(self.entries), 0)
            for key, mid in self._entries.items():
                self._write("<HQ", len(key), mid)
                self._write("<" + str(len(key)) + "s", key.encode("ascii"))
            self._f = None


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage:", sys.argv[0], "[path/to/iodi-metadata]", file=sys.stderr)
        sys.exit(1)
    metadata = IODIMetadata(sys.argv[1])
    json.dump(metadata.__dict__, sys.stdout, indent=2, sort_keys=True)
