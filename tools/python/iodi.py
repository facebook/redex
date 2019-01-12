#!/usr/bin/env python3

import json
import struct
import sys


class IODIMetadata(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            magic, version, single_count, double_count = struct.unpack(
                "<LLLL", f.read(4 * 4)
            )
            if magic != 0xFACEB001:
                raise Exception("Unexpected magic: " + hex(magic))
            if version != 1:
                raise Exception("Unexpected version: " + str(version))
            self.collision_free = {}
            self.collisions = {}
            for _ in range(single_count):
                klen, method_id = struct.unpack("<HQ", f.read(2 + 8))
                form = "<" + str(klen) + "s"
                key = struct.unpack(form, f.read(klen))[0].decode("ascii")
                self.collision_free[key] = method_id
            for _ in range(double_count):
                klen, count = struct.unpack("<HL", f.read(2 + 4))
                form = "<" + str(klen) + "s"
                key = struct.unpack(form, f.read(klen))[0].decode("ascii")
                dup_meta = {}
                for _ in range(count):
                    method_id, cs_count = struct.unpack("<QL", f.read(8 + 4))
                    mid_cs = {}
                    for _ in range(cs_count):
                        caller_method_id, pc = struct.unpack("<QH", f.read(8 + 2))
                        if caller_method_id not in mid_cs:
                            mid_cs[caller_method_id] = []
                        if pc in mid_cs[caller_method_id]:
                            raise Exception("Already found pc in pc to mid_cs")
                        mid_cs[caller_method_id].append(pc)
                    dup_meta[method_id] = mid_cs
                self.collisions[key] = dup_meta


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage:", sys.argv[0], "[path/to/iodi-metadata]", file=sys.stderr)
        sys.exit(1)
    metadata = IODIMetadata(sys.argv[1])
    json.dump(metadata.__dict__, sys.stdout, indent=2, sort_keys=True)
