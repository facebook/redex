#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

import argparse
import io
import logging
from contextlib import contextmanager
from typing import Any, Generator, List, NewType, Optional, Tuple

from lib.core import ReachabilityGraph, ReachableObjectType


class Buffer:
    def __init__(self) -> None:
        self._buffer = io.BytesIO()
        self._mark: int = -1

    def write_to(self, out: io.BytesIO) -> int:
        self._buffer.seek(0, io.SEEK_SET)
        buf = bytearray(4096)
        num_bytes = 0
        while True:
            buf_len = self._buffer.readinto(buf)
            if buf_len <= 0:
                break
            num_bytes += buf_len
            if buf_len == len(buf):
                out.write(buf)
            else:
                out.write(buf[:buf_len])

        return num_bytes

    def write(self, data: bytes) -> None:
        self._buffer.write(data)

    def writeU1(self, value: int) -> None:
        self._buffer.write(value.to_bytes(1, "big", signed=False))

    def writeU2(self, value: int) -> None:
        self._buffer.write(value.to_bytes(2, "big", signed=False))

    def writeU4(self, value: int) -> None:
        self._buffer.write(value.to_bytes(4, "big", signed=False))

    def tell(self) -> int:
        return self._buffer.tell()

    def update_u4(self, offset: int, value: int) -> None:
        self._buffer.seek(offset, io.SEEK_SET)
        self.writeU4(value)
        self._buffer.seek(0, io.SEEK_END)

    def start_record_with_mark(self, record_type: int) -> int:
        self.writeU1(record_type)
        self.writeU4(0x87654321)  # No time difference
        mark = self._buffer.tell()
        self.writeU4(0)  # Length, to be fixed up later
        return mark

    def start_record(self, record_type: int) -> None:
        self._mark = self.start_record_with_mark(record_type)

    def end_record_with_mark(self, mark: int) -> None:
        cur = self._buffer.tell()
        self._buffer.seek(mark, io.SEEK_SET)
        self.writeU4(cur - mark - 4)
        self._buffer.seek(cur, io.SEEK_SET)

    def end_record(self) -> None:
        assert self._mark > 0
        self.end_record_with_mark(self._mark)
        self._mark = -1

    @contextmanager
    def new_record(self, record_type: int) -> Generator[None, None, None]:
        mark = self.start_record_with_mark(record_type)
        yield
        self.end_record_with_mark(mark)


StringId = NewType("StringId", int)
ClassId = NewType("ClassId", int)
ObjectId = NewType("ObjectId", int)


class Hprof:
    TYPE_OBJECT: int = 2
    TYPE_BYTE: int = 8

    HPROF_GC_CLASS_DUMP: int = 0x20
    HPROF_GC_INSTANCE_DUMP: int = 0x21
    HPROF_GC_OBJ_ARRAY_DUMP: int = 0x22
    HPROF_GC_PRIM_ARRAY_DUMP: int = 0x23

    HPROF_GC_ROOT_UNKNOWN: int = 0xFF

    def __init__(self) -> None:
        self._buffer = Buffer()
        self._buffer.write(b"JAVA PROFILE 1.0.2\0")
        # IDs are 4 bytes long.
        self._buffer.writeU4(4)
        # Timestamp, leave at era.
        self._buffer.writeU4(0)
        self._buffer.writeU4(0)

        self._write_empty_stack_trace()

        self._strings: dict[str, StringId] = {}
        self._strings_buffer = Buffer()

        self._classes: dict[str, ClassId] = {}
        self._classes_buffer = Buffer()

        # pyre-ignore[4]
        self._objects: dict[Any, ObjectId] = {}
        self._objects_buffer = Buffer()
        self._objects_buffer_check: bool = False

        self._string_class: Optional[ClassId] = None

    def finish(self) -> None:
        self._strings_buffer.write_to(self._buffer._buffer)
        self._classes_buffer.write_to(self._buffer._buffer)
        with self._buffer.new_record(0x0C):
            self._objects_buffer.write_to(self._buffer._buffer)

    def to_bytes(self) -> bytes:
        return self._buffer._buffer.getvalue()

    def _write_empty_stack_trace(self) -> None:
        with self._buffer.new_record(0x05):
            self._buffer.writeU4(0)  # stack trace serial number
            self._buffer.writeU4(0)  # null thread
            self._buffer.writeU4(0)  # no frames

    def lookup_str(self, val: str) -> StringId:
        id = self._strings.get(val)
        if id:
            return id

        # Similar to Android.
        next_id = StringId(0x400000 + len(self._strings))
        self._strings[val] = next_id

        # A string record
        with self._strings_buffer.new_record(0x01):
            self._strings_buffer.writeU4(next_id)
            self._strings_buffer.write(val.encode())  # UTF-8

        return next_id

    def lookup_class(self, val: str) -> ClassId:
        id = self._classes.get(val)
        if id:
            return id

        next_id = ClassId(0x200000 + len(self._classes))
        self._classes[val] = next_id

        with self._classes_buffer.new_record(0x02):
            self._classes_buffer.writeU4(next_id)  # class serial
            self._classes_buffer.writeU4(next_id)  # object id
            self._classes_buffer.writeU4(0)  # stack trace serial
            self._classes_buffer.writeU4(self.lookup_str(val))  # class name string id

        return next_id

    @contextmanager
    def new_object(self, tag: int) -> Generator[Buffer, None, None]:
        assert not self._objects_buffer_check
        self._objects_buffer_check = True

        self._objects_buffer.writeU1(tag)
        yield self._objects_buffer

        self._objects_buffer_check = False

    @contextmanager
    def write_class(
        self, klass: str, superklass: Optional[str], instance_size: int
    ) -> Generator[Tuple[Buffer, ClassId], None, None]:
        with self.new_object(Hprof.HPROF_GC_CLASS_DUMP) as buf:
            class_id = self.lookup_class(klass)
            buf.writeU4(class_id)
            buf.writeU4(0)
            buf.writeU4(self.lookup_class(superklass) if superklass else 0)
            buf.writeU4(0)  # classloader
            buf.writeU4(0)  # signer
            buf.writeU4(0)  # protection domain
            buf.writeU4(0)  # reserved
            buf.writeU4(0)  # reserved
            buf.writeU4(instance_size)
            buf.writeU2(0)  # constant pool
            buf.writeU2(0)  # static fields
            yield buf, class_id

    def write_string_class(self) -> ClassId:
        id = self._string_class
        if id:
            return id

        with self.write_class("Ljava/lang/String;", None, 0) as (buf, class_id):
            # Just a "value" field pointing to data array
            buf.writeU2(1)
            buf.writeU4(self.lookup_str("value"))
            buf.writeU1(Hprof.TYPE_OBJECT)

            self._string_class = class_id
            return class_id

    # pyre-ignore[2]
    def next_object_id(self, key: Optional[Any] = None) -> ObjectId:
        id = ObjectId(0x600000 + len(self._objects))
        self._objects[key or id] = id
        return id

    @contextmanager
    def write_heap_instance(
        self, object_id: ObjectId, class_id: ClassId
    ) -> Generator[Buffer, None, None]:
        with self.new_object(Hprof.HPROF_GC_INSTANCE_DUMP) as buf:
            buf.writeU4(object_id)
            buf.writeU4(0)  # stacktrace serial
            buf.writeU4(class_id)
            # Length, needs to be patched
            len_pos = buf.tell()
            buf.writeU4(0)
            yield buf
            now_pos = buf.tell()
            buf.update_u4(len_pos, now_pos - len_pos - 4)

    def write_string(self, val: str) -> ObjectId:
        id = self._objects.get(val)
        if id:
            return id

        string_class_id = self.write_string_class()  # Ensure the class exists.

        # Dump the string content.
        primitive_id = self.write_byte_array(None, val.encode("utf-8"))

        next_id = self.next_object_id(val)
        # Now write the string object.
        with self.write_heap_instance(next_id, string_class_id) as buf:
            buf.writeU4(primitive_id)

        return next_id

    def write_byte_array(self, object_id: Optional[ObjectId], data: bytes) -> ObjectId:
        if not object_id:
            object_id = self.next_object_id()

        with self.new_object(Hprof.HPROF_GC_PRIM_ARRAY_DUMP) as buf:
            buf.writeU4(object_id)
            buf.writeU4(0)  # stacktrace serial
            buf.writeU4(len(data))
            buf.writeU1(Hprof.TYPE_BYTE)
            buf.write(data)

        return object_id

    def write_object_array(
        self, object_id: Optional[ObjectId], data: List[ObjectId]
    ) -> ObjectId:
        if not object_id:
            object_id = self.next_object_id()

        with self.new_object(Hprof.HPROF_GC_OBJ_ARRAY_DUMP) as buf:
            buf.writeU4(object_id)
            buf.writeU4(0)  # stacktrace serial
            buf.writeU4(len(data))
            buf.writeU4(self.lookup_class("[Ljava/lang/Object;"))
            for id in data:
                buf.writeU4(id)

        return object_id

    def make_root(self, object_id: ObjectId) -> None:
        with self.new_object(Hprof.HPROF_GC_ROOT_UNKNOWN) as buf:
            buf.writeU4(object_id)


def write_keep_class(hprof: Hprof) -> ClassId:
    with hprof.write_class("LKeep;", None, 0) as (buf, class_id):
        # A field for the name, and a field for the array holding successors.
        buf.writeU2(2)

        buf.writeU4(hprof.lookup_str("name"))
        buf.writeU1(Hprof.TYPE_OBJECT)

        buf.writeU4(hprof.lookup_str("succs"))
        buf.writeU1(Hprof.TYPE_OBJECT)

    return class_id


def write_keep_instance(hprof: Hprof, name: str, succs: List[ObjectId]) -> ObjectId:
    # First write the succs and name.
    succs_id = hprof.write_object_array(None, succs)

    name_id = hprof.write_string(name)

    next_id = hprof.next_object_id()

    # Then write the keep instance.
    with hprof.write_heap_instance(next_id, hprof.lookup_class("LKeep;")) as buf:
        buf.writeU4(name_id)
        buf.writeU4(succs_id)

    return next_id


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="""A tool to translate a grpah generated by Redex's Reachability service to an HPROF file.
It only supports ReachabilityGraph for now.""",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument(
        "input", help="Input file generated by Reachability.dump_graph()."
    )
    parser.add_argument("output", help="The hprof file that should be generated.")

    return parser.parse_args()


def main() -> None:
    args = parse_args()
    graph = ReachabilityGraph()

    logging.info(
        "Loading graph from %s. This might take a couple of minutes...", args.input
    )
    graph.load(args.input)

    logging.info("Creating HPROF representation...")

    hprof = Hprof()

    obj1 = hprof.write_string("Hello World")
    hprof.write_string("Hm")

    hprof.make_root(obj1)

    write_keep_class(hprof)

    seeds = 0
    for (node_type, _), node in graph.nodes.items():
        if node_type != ReachableObjectType.SEED:
            continue

        keep_id = write_keep_instance(hprof, node.name, [])
        hprof.make_root(keep_id)

        seeds += 1
    logging.info("Found %d seeds", seeds)

    hprof.finish()

    logging.info("Writing to %s...", args.output)

    with open(args.output, "wb") as f:
        f.write(hprof.to_bytes())


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    main()
