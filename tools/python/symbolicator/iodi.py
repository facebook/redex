#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import struct
import sys


class IODIMetadata(object):
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

    def map_iodi(
        self, debug_line_map, class_name, method_name, input_lineno, input_method_id
    ):
        qualified_name = class_name + "." + method_name
        if qualified_name in self._entries:
            method_id = self._entries[qualified_name]
            mapped = debug_line_map.find_line_number(method_id, input_lineno)
            if mapped is not None:
                return (mapped, method_id)
            return (None, method_id)
        return (None, None)

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
