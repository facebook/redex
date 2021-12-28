#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Parses Android hprof dumps.
# Example usage:
# In [1]: import hprof
# In [2]: hprof.parse_filename('/Users/tcm/Documents/com.facebook.crudo.hprof')
# Out[3]: <HprofData TAG="JAVA PROFILE 1.0.3" id-size=4 timestamp=1406233374264>
# In [4]: hp = _
# In [5]: hprof.open_bitmaps(hp.lookup_instances_of_class('android.graphics.Bitmap'))
# Writing 281 bitmaps to /var/folders/1g/1dxc_1rd1jg5l2csn_tck2085w5qnj/T/tmpwMwbMCbitmaps.
# 281 of 281 complete

import argparse
import enum
import logging
import os.path
import struct
import subprocess
import sys
import tempfile
from array import array
from collections import defaultdict


# Allow missing IDs in some cases to work around seemingly broken hprofs.
allow_missing_ids = False


def parse_hprof_dump(instream):
    # Read the tag - a null-terminated string
    tag = b""
    while True:
        byte = instream.read(1)
        if not byte:
            break
        if byte == b"\x00":
            break
        tag += byte
    tag = tag.decode("utf-8")  # UTF8 should be close enough to modified UTF8.

    big_endian_unsigned_4byte_integer = struct.Struct(b">I")
    sizeof_id = big_endian_unsigned_4byte_integer.unpack(instream.read(4))[0]

    high_timestamp = big_endian_unsigned_4byte_integer.unpack(instream.read(4))[0]
    low_timestamp = big_endian_unsigned_4byte_integer.unpack(instream.read(4))[0]
    timestamp = (high_timestamp << 32) | low_timestamp

    hprof_data = HprofData(tag, sizeof_id, timestamp)
    while True:
        record = Record.read_from_stream(hprof_data, instream)
        if record.tag == HprofTag.HEAP_DUMP_END:
            break

    hprof_data.resolve()

    return hprof_data


def parse_file(instream):
    return parse_hprof_dump(instream)


def parse_filename(filename):
    return parse_hprof_dump(open(filename, "rb"))


class HprofTag(enum.Enum):
    STRING = 0x01
    LOAD_CLASS = 0x02
    UNLOAD_CLASS = 0x03
    STACK_FRAME = 0x04
    STACK_TRACE = 0x05
    ALLOC_SITES = 0x06
    HEAP_SUMMARY = 0x07
    START_THREAD = 0x0A
    END_THREAD = 0x0B
    HEAP_DUMP = 0x0C
    HEAP_DUMP_SEGMENT = 0x1C
    HEAP_DUMP_END = 0x2C
    CPU_SAMPLES = 0x0D
    CONTROL_SETTINGS = 0x0E


class HeapTag(enum.Enum):
    # standard
    ROOT_UNKNOWN = 0xFF
    ROOT_JNI_GLOBAL = 0x01
    ROOT_JNI_LOCAL = 0x02
    ROOT_JAVA_FRAME = 0x03
    ROOT_NATIVE_STACK = 0x04
    ROOT_STICKY_CLASS = 0x05
    ROOT_THREAD_BLOCK = 0x06
    ROOT_MONITOR_USED = 0x07
    ROOT_THREAD_OBJECT = 0x08
    CLASS_DUMP = 0x20
    INSTANCE_DUMP = 0x21
    OBJECT_ARRAY_DUMP = 0x22
    PRIMITIVE_ARRAY_DUMP = 0x23

    # Android
    HEAP_DUMP_INFO = 0xFE
    ROOT_INTERNED_STRING = 0x89
    ROOT_FINALIZING = 0x8A  # obsolete
    ROOT_DEBUGGER = 0x8B
    ROOT_REFERENCE_CLEANUP = 0x8C  # obsolete
    ROOT_VM_INTERNAL = 0x8D
    ROOT_JNI_MONITOR = 0x8E
    UNREACHABLE = 0x90  # obsolete
    PRIMITIVE_ARRAY_NODATA_DUMP = 0xC3


class HprofBasic(enum.Enum):
    OBJECT = 2
    BOOLEAN = 4
    CHAR = 5
    FLOAT = 6
    DOUBLE = 7
    BYTE = 8
    SHORT = 9
    INT = 10
    LONG = 11

    def size(self):
        if self is HprofBasic.OBJECT:
            return 4
        elif self is HprofBasic.BOOLEAN:
            return 1
        elif self is HprofBasic.CHAR:
            return 2
        elif self is HprofBasic.FLOAT:
            return 4
        elif self is HprofBasic.DOUBLE:
            return 8
        elif self is HprofBasic.BYTE:
            return 1
        elif self is HprofBasic.SHORT:
            return 2
        elif self is HprofBasic.INT:
            return 4
        elif self is HprofBasic.LONG:
            return 8
        else:
            raise Exception("Invalid HprofBasic type: %s" % self)

    def parse(self, byte_stream):
        if self is HprofBasic.OBJECT:
            return byte_stream.next_id()
        elif self is HprofBasic.BOOLEAN:
            return byte_stream.next_byte() != 0
        elif self is HprofBasic.CHAR:
            return byte_stream.next_two_bytes()
        elif self is HprofBasic.FLOAT:
            return byte_stream.next_four_bytes()
        elif self is HprofBasic.DOUBLE:
            return byte_stream.next_eight_bytes()
        elif self is HprofBasic.BYTE:
            return byte_stream.next_byte()
        elif self is HprofBasic.SHORT:
            return byte_stream.next_two_bytes()
        elif self is HprofBasic.INT:
            return byte_stream.next_four_bytes()
        elif self is HprofBasic.LONG:
            return byte_stream.next_eight_bytes()
        else:
            raise Exception("Invalid HprofBasic type: %s" % self)


class Record(object):
    record_struct_format = b">BII"
    record_struct = struct.Struct(record_struct_format)

    def __init__(self, tag, time_offset_us):
        self.tag = tag
        self.time_offset_us = time_offset_us

    @staticmethod
    def read_from_stream(hprof_data, instream):
        (tag, time_offset_us, length) = Record.record_struct.unpack(
            instream.read(struct.calcsize(Record.record_struct_format))
        )
        data = array("B")
        data.frombytes(instream.read(length))
        if tag == HprofTag.STRING.value:
            return hprof_data.parse_string_record(
                tag=tag, time_offset_us=time_offset_us, data=data
            )
        elif tag == HprofTag.LOAD_CLASS.value:
            return hprof_data.parse_load_class_record(
                tag=tag, time_offset_us=time_offset_us, data=data
            )
        elif tag == HprofTag.HEAP_DUMP_SEGMENT.value:
            return hprof_data.parse_heap_dump_segment_record(
                tag=tag, time_offset_us=time_offset_us, data=data
            )

        # default
        record = Record(tag=HprofTag(tag), time_offset_us=time_offset_us)
        record.length = length
        record.data = data
        return record

    def __str__(self):
        return "Record { %s %dus %d }" % (self.tag, self.time_offset_us, self.length)


class StringRecord(Record):
    def __init__(self, tag, time_offset_us, string_id, string):
        super(StringRecord, self).__init__(tag, time_offset_us)
        self.string_id = string_id
        self.string = string

    def __str__(self):
        return "StringRecord { %dus %s }" % (self.time_offset_us, self.string)

    @staticmethod
    def create(tag, time_offset_us, data):
        byte_stream = ByteStream(data)
        heap_id = byte_stream.next_four_bytes()
        string = byte_stream.remainder().tobytes().decode("utf-8")
        return StringRecord(tag, time_offset_us, heap_id, string)


class LoadClassRecord(Record):
    def __init__(
        self,
        tag,
        time_offset_us,
        class_serial,
        object_id,
        stack_serial,
        class_string_id,
    ):
        super(LoadClassRecord, self).__init__(tag, time_offset_us)
        self.class_serial = class_serial
        self.object_id = object_id
        self.stack_serial = stack_serial
        self.class_string_id = class_string_id

    def __str__(self):
        return "LoadClassRecord { %dus %d %d %d %d }" % (
            self.time_offset_us,
            self.class_serial,
            self.object_id,
            self.stack_serial,
            self.class_string_id,
        )

    @staticmethod
    def create(tag, time_offset_us, data):
        byte_stream = ByteStream(data)
        class_serial = byte_stream.next_four_bytes()
        object_id = byte_stream.next_id()
        stack_serial = byte_stream.next_four_bytes()
        class_string_id = byte_stream.next_id()
        assert not byte_stream.has_more()

        return LoadClassRecord(
            tag=tag,
            time_offset_us=time_offset_us,
            class_serial=class_serial,
            object_id=object_id,
            stack_serial=stack_serial,
            class_string_id=class_string_id,
        )


class ByteStream(object):
    def __init__(self, data):
        self.data = data
        self.index = 0

    def next_byte(self):
        byte = self.data[self.index]
        self.index += 1
        return byte

    def next_two_bytes(self):
        two_bytes = struct.unpack(b">H", self.data[self.index : self.index + 2])[0]
        self.index += 2
        return two_bytes

    def next_four_bytes(self):
        four_bytes = struct.unpack(b">I", self.data[self.index : self.index + 4])[0]
        self.index += 4
        return four_bytes

    def next_eight_bytes(self):
        eight_bytes = struct.unpack(b">Q", self.data[self.index : self.index + 8])[0]
        self.index += 8
        return eight_bytes

    # TODO: support 64-bit
    def next_id(self):
        return self.next_four_bytes()

    def next_byte_array(self, length):
        byte_array = self.data[self.index : self.index + length]
        self.index += length
        return byte_array

    def remainder(self):
        index = self.index
        self.index = len(self.data)
        return self.data[index:]

    def has_more(self):
        return self.index < len(self.data) - 1


class HeapDumpSegmentRecord(Record):
    def __init__(self, tag, time_offset_us):
        super(HeapDumpSegmentRecord, self).__init__(tag, time_offset_us)

    def __str__(self):
        return "HeapDumpSegmentRecord"


class HprofHeap(object):
    def __init__(self, heap_id, name_string_id):
        self.heap_id = heap_id
        self.name_string_id = name_string_id

    def resolve(self, hprof_data):
        self.name = hprof_data.lookup_string(self.name_string_id)
        del self.name_string_id

    def __str__(self):
        return "<HprofHeap %s>" % self.name

    def __repr__(self):
        return str(self)


class SimpleSegment(object):
    def __init__(self, heap_tag, object_id):
        self.heap_tag = heap_tag
        self.object_id = object_id


class HprofRoot(SimpleSegment):
    def __init__(self, heap_tag, object_id):
        super(HprofRoot, self).__init__(heap_tag, object_id)

    def resolve(self, hprof_data):
        self.obj = hprof_data.resolve_object_id(self.object_id)
        del self.object_id

    def __str__(self):
        return "<HprofRoot %s %s>" % (self.heap_tag, self.obj)

    def __repr__(self):
        return str(self)


class StaticField(object):
    def __init__(self, string_id, hprof_basic, value):
        self.string_id = string_id
        self.hprof_basic = hprof_basic
        self.value = value

    @staticmethod
    def parse(byte_stream):
        static_field_name_string_id = byte_stream.next_four_bytes()
        hprof_basic = HprofBasic(byte_stream.next_byte())
        value = hprof_basic.parse(byte_stream)
        return StaticField(static_field_name_string_id, hprof_basic, value)

    def resolve(self, hprof_data):
        self.name = hprof_data.lookup_string(self.string_id)
        del self.string_id


class InstanceField(object):
    def __init__(self, string_id, hprof_basic):
        self.string_id = string_id
        self.hprof_basic = hprof_basic

    @staticmethod
    def parse(byte_stream):
        field_name_string_id = byte_stream.next_four_bytes()
        hprof_basic = HprofBasic(byte_stream.next_byte())
        return InstanceField(field_name_string_id, hprof_basic)

    def resolve(self, hprof_data):
        self.name = hprof_data.lookup_string(self.string_id)
        del self.string_id


class ReferenceType(enum.Enum):
    SUPER_CLASS = 2
    CLASS_LOADER = 3
    CLASS = 4
    FIELD = 5
    ARRAY = 6


class Reference(object):
    def __init__(self, reference_type, referer, referee):
        assert isinstance(referer, HprofObject)
        assert isinstance(referee, HprofObject)
        self.reference_type = reference_type
        self.referer = referer
        self.referee = referee

    def __str__(self):
        return "<Reference %s %s %s>" % (
            self.reference_type,
            self.referer,
            self.referee,
        )

    def __repr__(self):
        return str(self)


class FieldReference(Reference):
    def __init__(self, referer, referee, class_name, field_name):
        super(FieldReference, self).__init__(ReferenceType.FIELD, referer, referee)
        self.class_name = class_name
        self.field_name = field_name

    def __str__(self):
        return "<FieldReference %s.%s %s>" % (
            self.referer,
            self.field_name,
            self.referee,
        )


class HprofObject(SimpleSegment):
    def __init__(self, heap_tag, object_id, heap_id):
        super(HprofObject, self).__init__(heap_tag, object_id)
        self.heap_id = heap_id

    def resolve(self, hprof_data):
        self.hprof_data = hprof_data

        if self.heap_id is None:
            self.heap = None
        else:
            self.heap = hprof_data.lookup_heap(self.heap_id)
        del self.heap_id

    def incoming_references(self):
        return self.hprof_data.lookup_references(self)

    def __str__(self):
        return "<%s 0x%x>" % (self.__class__.__name__, self.object_id)

    def __repr__(self):
        return str(self)


class MergedFields(object):
    def __init__(self):
        pass


class HprofInstance(HprofObject):
    def __init__(self, heap_tag, object_id, heap_id):
        super(HprofInstance, self).__init__(heap_tag, object_id, heap_id)

    def resolve(self, hprof_data):
        super(HprofInstance, self).resolve(hprof_data)

        # load the class of this instance
        self.clazz = hprof_data.resolve_object_id(self.class_object_id)
        del self.class_object_id

        # To avoid over-writing shadowed fields, we have nested dicts, one for each
        # class in the hierarchy
        self.class_fields = defaultdict(dict)

        # for convenience
        merged_fields_builder = defaultdict(dict)
        self.fields = MergedFields()

        byte_stream = ByteStream(self.instance_field_data)
        del self.instance_field_data

        # Instance field data consists of current class data, followed by super
        # class data, and so on.
        clazz = self.clazz
        while clazz is not None:
            for field in clazz.instance_fields:
                value = field.hprof_basic.parse(byte_stream)
                name = field.name

                if field.hprof_basic is HprofBasic.OBJECT:
                    value = hprof_data.resolve_object_id(value)

                self.class_fields[clazz.name][name] = value
                merged_fields_builder[name][clazz.name] = value
            clazz = clazz.super_class

        for key, value in merged_fields_builder.items():
            # Avoid over-writing python internals, like __dict__
            if key in self.fields.__dict__:
                key = "__hprof_" + key
                assert key not in self.fields.__dict__

            if len(value) == 1:
                setattr(self.fields, key, next(iter(value.values())))
            else:
                # There is a conflict in the class hierarchy (e.g. privates with the
                # same name), so we need to store a dictionary.
                setattr(self.fields, key, value)

        if byte_stream.has_more():
            raise Exception("Extra data in %d" % self.object_id)

    def outgoing_references_to(self, obj):
        return self.outgoing_references(lambda x: x is obj)

    def outgoing_references(self, filter_function=lambda x: True):
        # we are not walking the references from the class even though an instance
        # can be thought as referencing its class. That ends up being too intrusive
        # and attribute weight the wrong way.
        # Classes should be walked explicitly
        refs = []
        for class_name, fields in self.class_fields.items():
            for name, value in fields.items():
                if isinstance(value, HprofObject) and filter_function(value):
                    refs.append(FieldReference(self, value, class_name, name))
        return refs

    def __str__(self):
        return "<%s %s 0x%x>" % (
            self.__class__.__name__,
            self.clazz.name,
            self.object_id,
        )

    def __repr__(self):
        return str(self)

    def shallow_size(self):
        # This should be pretty exact
        return self.clazz.instance_size


class HprofClass(HprofObject):
    def __init__(self, object_id, heap_id):
        super(HprofClass, self).__init__(HeapTag.CLASS_DUMP, object_id, heap_id)
        self.children = []

    @staticmethod
    def parse(byte_stream, heap_id):
        segment = HprofClass(byte_stream.next_id(), heap_id)

        segment.stack_serial = byte_stream.next_four_bytes()
        segment.super_class_id = byte_stream.next_id()
        segment.class_loader_id = byte_stream.next_id()
        segment.signer = byte_stream.next_id()  # always zero on dalvik
        segment.prot_domain = byte_stream.next_id()  # always zero on dalvik

        # reserved
        byte_stream.next_id()
        byte_stream.next_id()

        segment.instance_size = byte_stream.next_four_bytes()
        segment.const_pool_count = (
            byte_stream.next_two_bytes()
        )  # always empty on dalvik
        if segment.const_pool_count > 0:
            raise Exception("Cannot handle const_pools.")

        static_field_count = byte_stream.next_two_bytes()
        segment.static_fields = []

        for _ in range(static_field_count):
            segment.static_fields.append(StaticField.parse(byte_stream))

        instance_field_count = byte_stream.next_two_bytes()
        segment.instance_fields = []

        for _ in range(instance_field_count):
            segment.instance_fields.append(InstanceField.parse(byte_stream))

        return segment

    def resolve(self, hprof_data):
        super(HprofClass, self).resolve(hprof_data)

        load_class_record = hprof_data.lookup_load_class_record(self.object_id)
        self.name = hprof_data.lookup_string(load_class_record.class_string_id)

        if self.super_class_id > 0:
            self.super_class = hprof_data.resolve_object_id(self.super_class_id)
            self.super_class.children.append(self)
        else:
            self.super_class = None
        del self.super_class_id

        if self.class_loader_id > 0:
            self.class_loader = hprof_data.resolve_object_id(self.class_loader_id)
        else:
            self.class_loader = None
        del self.class_loader_id

        for field in self.instance_fields:
            field.resolve(hprof_data)

        self.fields = MergedFields()
        for static_field in self.static_fields:
            static_field.resolve(hprof_data)
            if static_field.hprof_basic == HprofBasic.OBJECT:
                static_field.value = hprof_data.resolve_object_id(static_field.value)
            name = static_field.name

            # Don't want to overwrite Python internal fields - like __dict__
            if name in self.fields.__dict__:
                name = "__hprof_" + name
                assert name not in self.fields.__dict__
            setattr(self.fields, name, static_field.value)

    def outgoing_references_to(self, obj):
        return self.outgoing_references(lambda x: x is obj)

    def outgoing_references(self, filter_function=lambda x: True):
        refs = []
        if self.super_class is not None and filter_function(self.super_class):
            refs.append(Reference(ReferenceType.SUPER_CLASS, self, self.super_class))
        if self.class_loader is not None and filter_function(self.class_loader):
            refs.append(Reference(ReferenceType.CLASS_LOADER, self, self.class_loader))

        for static_field in self.static_fields:
            if isinstance(static_field.value, HprofObject) and filter_function(
                static_field.value
            ):
                refs.append(
                    FieldReference(
                        self, static_field.value, self.name, static_field.name
                    )
                )

        return refs

    def __str__(self):
        return "<Class %s>" % self.name

    def shallow_size(self):
        # This is an estimate
        # One id for the class (i.e. java.lang.Class)
        # One id for the lock
        # I counted 34 id members before the static array
        # 5 ids for each static field
        return 36 * self.hprof_data.sizeof_id + 5 * self.hprof_data.sizeof_id * len(
            self.static_fields
        )


class HprofPrimitiveArray(HprofObject):
    def __init__(self, object_id, heap_id):
        super(HprofPrimitiveArray, self).__init__(
            HeapTag.PRIMITIVE_ARRAY_DUMP, object_id, heap_id
        )

    @staticmethod
    def parse(byte_stream, heap_id):
        segment = HprofPrimitiveArray(byte_stream.next_id(), heap_id)

        segment.stack_serial = byte_stream.next_four_bytes()
        segment.num_elements = byte_stream.next_four_bytes()
        segment.prim_type = HprofBasic(byte_stream.next_byte())

        # Parsing primitive data is slow, and not always so interesting, so we defer
        segment.array_data = byte_stream.next_byte_array(
            segment.num_elements * segment.prim_type.size()
        )

        return segment

    def resolve(self, hprof_data):
        super(HprofPrimitiveArray, self).resolve(hprof_data)
        self.clazz = hprof_data.class_name_dict[self.prim_type.name.lower() + "[]"]

    # Resolving large arrays is expensive and pointless
    def fully_resolve(self):
        self.array_values = []
        byte_stream = ByteStream(self.array_data)
        count = int(len(self.array_data) / self.prim_type.size())
        for _ in range(count):
            self.array_values.append(self.prim_type.parse(byte_stream))
        # We keep around array_data since it's sometimes handy - like when loading bitmaps

    def outgoing_references_to(self, obj):
        return []

    def outgoing_references(self, filter_function=lambda x: True):
        return []

    def shallow_size(self):
        # This is an estimate
        # One id for the pointer to the class object
        # One id for the lock
        # One prim for each of the array slots
        return 2 * self.hprof_data.sizeof_id + self.num_elements * self.prim_type.size()


class HprofObjectArray(HprofObject):
    def __init__(self, object_id, heap_id):
        super(HprofObjectArray, self).__init__(
            HeapTag.OBJECT_ARRAY_DUMP, object_id, heap_id
        )

    @staticmethod
    def parse(byte_stream, heap_id):
        segment = HprofObjectArray(byte_stream.next_id(), heap_id)
        segment.stack_serial = byte_stream.next_four_bytes()
        num_elements = byte_stream.next_four_bytes()
        segment.array_class_object_id = byte_stream.next_four_bytes()

        segment.array_values = []
        for _ in range(num_elements):
            segment.array_values.append(byte_stream.next_id())

        return segment

    def resolve(self, hprof_data):
        super(HprofObjectArray, self).resolve(hprof_data)

        self.clazz = hprof_data.resolve_object_id(self.array_class_object_id)

        for i, obj in enumerate(self.array_values):
            # Resolve non-null value to parsed instance
            if obj != 0:
                self.array_values[i] = hprof_data.resolve_object_id(
                    obj, "No object for %x (index %d in %x)", obj, i, self.object_id
                )
            else:
                self.array_values[i] = None

    def outgoing_references_to(self, obj):
        return self.outgoing_references(lambda x: x is obj)

    def outgoing_references(self, filter_function=lambda x: True):
        refs = []
        for value in self.array_values:
            if value is not None and filter_function(value):
                refs.append(Reference(ReferenceType.ARRAY, self, value))
        return refs

    def shallow_size(self):
        # This is an estimate
        # One id for the pointer to the class object
        # One id for the lock
        # One id for each of the array slots
        return 2 * self.hprof_data.sizeof_id + self.hprof_data.sizeof_id * len(
            self.array_values
        )


class HprofString(HprofInstance):
    def __init__(self, heap_tag, object_id, heap_id):
        super(HprofString, self).__init__(heap_tag, object_id, heap_id)

    def string(self):
        char_array = self.class_fields["java.lang.String"]["value"]
        char_array.fully_resolve()
        return "".join(
            [chr(char_array.array_values[i]) for i in range(self.fields.count)]
        )

    def __str__(self):
        return "<String %s 0x%x count=%d>" % (
            self.clazz.name,
            self.object_id,
            self.fields.count,
        )


class HprofData(object):
    def __init__(self, tag, sizeof_id, timestamp):
        self.tag = tag
        self.sizeof_id = sizeof_id
        self.timestamp = timestamp

        self.object_id_dict = {}
        self.string_id_dict = {}
        self.class_object_id_to_load_class_record = {}
        self.roots = []
        self.heap_dict = {}

        # Populated in resolve step
        self.class_name_dict = {}

        self.dupe_class_dict = defaultdict(list)

        self.current_heap_id = None
        self.string_class_object_id = None

        self.inverted_references = None
        self.gc_done = False

    def __str__(self):
        return '<HprofData TAG="%s" id-size=%d timestamp=%d>' % (
            self.tag,
            self.sizeof_id,
            self.timestamp,
        )

    def __repr__(self):
        return str(self)

    def parse_class_dump(self, byte_stream):
        clazz = HprofClass.parse(byte_stream, self.current_heap_id)

        # Kind of hacky, but allows derived instance types
        load_class_record = self.lookup_load_class_record(clazz.object_id)
        name = self.lookup_string(load_class_record.class_string_id)
        if name == "java.lang.String":
            assert self.string_class_object_id is None
            self.string_class_object_id = clazz.object_id

        self.object_id_dict[clazz.object_id] = clazz
        return clazz

    def parse_instance_dump(self, byte_stream):
        object_id = byte_stream.next_id()
        stack_serial = byte_stream.next_four_bytes()
        class_object_id = byte_stream.next_id()
        instance_field_values_size = byte_stream.next_four_bytes()
        instance_field_data = byte_stream.next_byte_array(instance_field_values_size)

        if class_object_id == self.string_class_object_id:
            segment = HprofString(
                HeapTag.INSTANCE_DUMP, object_id, self.current_heap_id
            )
        else:
            segment = HprofInstance(
                HeapTag.INSTANCE_DUMP, object_id, self.current_heap_id
            )

        segment.stack_serial = stack_serial
        segment.class_object_id = class_object_id
        segment.instance_field_data = instance_field_data

        if segment.object_id in self.object_id_dict:
            raise Exception("Duplicate object_id: %d" % segment.object_id)

        self.object_id_dict[segment.object_id] = segment
        return segment

    def parse_primitive_array_dump(self, byte_stream):
        primitive_array = HprofPrimitiveArray.parse(byte_stream, self.current_heap_id)
        if primitive_array.object_id in self.object_id_dict:
            raise Exception("Duplicate object_id: %d" % primitive_array.object_id)

        self.object_id_dict[primitive_array.object_id] = primitive_array
        return primitive_array

    def parse_object_array_dump(self, byte_stream):
        object_array = HprofObjectArray.parse(byte_stream, self.current_heap_id)
        if object_array.object_id in self.object_id_dict:
            raise Exception("Duplicate object_id: %d" % object_array.object_id)

        self.object_id_dict[object_array.object_id] = object_array
        return object_array

    def parse_string_record(self, tag, time_offset_us, data):
        string_record = StringRecord.create(tag, time_offset_us, data)
        self.string_id_dict[string_record.string_id] = string_record
        return string_record

    def parse_load_class_record(self, tag, time_offset_us, data):
        load_class_record = LoadClassRecord.create(tag, time_offset_us, data)
        self.class_object_id_to_load_class_record[
            load_class_record.object_id
        ] = load_class_record
        return load_class_record

    def parse_heap_dump_segment_record(self, tag, time_offset_us, data):
        byte_stream = ByteStream(data)

        while byte_stream.has_more():
            heap_tag = HeapTag(byte_stream.next_byte())
            if heap_tag in (
                HeapTag.ROOT_UNKNOWN,
                HeapTag.ROOT_STICKY_CLASS,
                HeapTag.ROOT_MONITOR_USED,
                HeapTag.ROOT_INTERNED_STRING,
                HeapTag.ROOT_FINALIZING,
                HeapTag.ROOT_DEBUGGER,
                HeapTag.ROOT_REFERENCE_CLEANUP,
                HeapTag.ROOT_VM_INTERNAL,
            ):
                object_id = byte_stream.next_four_bytes()
                hprof_root = HprofRoot(heap_tag, object_id)

                self.add_root(hprof_root)
            elif heap_tag is HeapTag.ROOT_JNI_GLOBAL:
                object_id = byte_stream.next_id()
                hprof_root = HprofRoot(heap_tag, object_id)
                hprof_root.jni_global_ref_id = byte_stream.next_id()

                self.add_root(hprof_root)
            elif heap_tag is HeapTag.ROOT_THREAD_OBJECT:
                thread_object_id = byte_stream.next_id()
                hprof_root = HprofRoot(heap_tag, thread_object_id)
                hprof_root.thread_serial = byte_stream.next_four_bytes()
                hprof_root.stack_serial = byte_stream.next_four_bytes()

                self.add_root(hprof_root)
            elif heap_tag in (
                HeapTag.ROOT_JNI_LOCAL,
                HeapTag.ROOT_JNI_MONITOR,
                HeapTag.ROOT_JAVA_FRAME,
            ):
                object_id = byte_stream.next_id()
                hprof_root = HprofRoot(heap_tag, object_id)
                hprof_root.thread_serial = byte_stream.next_four_bytes()
                hprof_root.stack_serial = byte_stream.next_four_bytes()

                self.add_root(hprof_root)
            elif heap_tag in (HeapTag.ROOT_NATIVE_STACK, HeapTag.ROOT_THREAD_BLOCK):
                object_id = byte_stream.next_id()
                hprof_root = HprofRoot(heap_tag, object_id)
                hprof_root.thread_serial = byte_stream.next_four_bytes()

                self.add_root(hprof_root)
            elif heap_tag is HeapTag.HEAP_DUMP_INFO:
                heap_id = byte_stream.next_id()
                name_string_id = byte_stream.next_id()

                self.add_heap(HprofHeap(heap_id, name_string_id))
            elif heap_tag is HeapTag.PRIMITIVE_ARRAY_DUMP:
                self.parse_primitive_array_dump(byte_stream)
            elif heap_tag is HeapTag.CLASS_DUMP:
                # print("skipping class dump")
                self.parse_class_dump(byte_stream)
            elif heap_tag is HeapTag.INSTANCE_DUMP:
                # print("skipping instance dump")
                self.parse_instance_dump(byte_stream)
            elif heap_tag is HeapTag.OBJECT_ARRAY_DUMP:
                # print("skipping obj array dump")
                self.parse_object_array_dump(byte_stream)
            else:
                raise Exception("Unrecognized tag: %s" % heap_tag)

        return HeapDumpSegmentRecord(tag, time_offset_us)

    def add_heap(self, hprof_heap):
        if hprof_heap.heap_id in self.heap_dict:
            existing_heap = self.heap_dict[hprof_heap.heap_id]
            assert hprof_heap.heap_id == existing_heap.heap_id
            assert hprof_heap.name_string_id == existing_heap.name_string_id
        else:
            self.heap_dict[hprof_heap.heap_id] = hprof_heap

        self.current_heap_id = hprof_heap.heap_id

    def add_root(self, hprof_root):
        self.roots.append(hprof_root)

    def resolve(self):
        # First resolve heaps
        for heap in self.heap_dict.values():
            heap.resolve(self)

        # Then resolve classes
        for obj in self.object_id_dict.values():
            if isinstance(obj, HprofClass):
                clazz = obj
                clazz.resolve(self)
                if clazz.name in self.class_name_dict:
                    if (
                        self.class_name_dict[clazz.name]
                        not in self.dupe_class_dict[clazz.name]
                    ):
                        self.dupe_class_dict[clazz.name].append(
                            self.class_name_dict[clazz.name]
                        )
                    self.dupe_class_dict[clazz.name].append(clazz)
                    print("Warning: duplicate class: %s" % clazz.name)
                else:
                    self.class_name_dict[clazz.name] = clazz
        # Fix up all classes to derive from java.lang.Class
        # at the time we create every HprofClass 'java.lang.Class' may have
        # not be parsed yet and thus unavailable
        clsCls = self.class_name_dict["java.lang.Class"]
        for cls in self.class_name_dict.values():
            cls.clazz = clsCls

        # Then other objects
        for obj in self.object_id_dict.values():
            if not isinstance(obj, HprofClass):
                obj.resolve(self)
            obj.is_root = False  # Fixed up for root objects below

        # Then roots
        for root in self.roots:
            root.resolve(self)
            root.obj.is_root = True

    def resolve_object_id(self, obj_id, fmt=None, *args):
        if obj_id == 0:
            return None
        if obj_id in self.object_id_dict:
            return self.object_id_dict[obj_id]
        if allow_missing_ids:
            if fmt is not None:
                logging.warning(fmt, *args)
            return None
        if fmt is not None:
            raise RuntimeError(fmt % args)
        raise RuntimeError(f"No object for {obj_id:x}")

    def lookup_string(self, string_id):
        return self.string_id_dict[string_id].string

    def lookup_load_class_record(self, class_object_id):
        return self.class_object_id_to_load_class_record[class_object_id]

    def lookup_instances_of_class(self, class_name):
        return [
            obj
            for obj in self.object_id_dict.values()
            if isinstance(obj, HprofInstance) and obj.clazz.name == class_name
        ]

    def load_inverted_references(self):
        if self.inverted_references is None:
            # Will be much faster for later invocations
            self.inverted_references = defaultdict(list)
            for heap_obj in self.object_id_dict.values():
                for ref in heap_obj.outgoing_references():
                    self.inverted_references[ref.referee].append(ref)

    def gc(self):
        if not self.gc_done:
            self.gc_done = True
            live_instances = {}
            incoming_references = defaultdict(list)
            current_list = set()
            for root in self.roots:
                current_list.add(root.obj)
                live_instances[root.obj.object_id] = root.obj
            while len(current_list) > 0:
                new_current = set()
                for obj in current_list:
                    for ref in obj.outgoing_references():
                        curr = ref.referee
                        incoming_references[curr].append(ref)
                        if curr.object_id not in live_instances:
                            new_current.add(curr)
                            live_instances[curr.object_id] = curr
                current_list = new_current
            self.object_id_dict = live_instances
            self.inverted_references = incoming_references

    def lookup_references(self, obj):
        self.load_inverted_references()
        return self.inverted_references[obj]

    def lookup_heap(self, heap_id):
        return self.heap_dict[heap_id]


def strings(hprof_data):
    string_instances = hprof_data.lookup_instances_of_class("java.lang.String")
    return [instance.string() for instance in string_instances]


def app_strings(hprof_data):
    return [
        instance.string()
        for instance in app_string_instances(hprof_data)
        if instance.heap.name != "zygote"
    ]


def app_string_instances(hprof_data):
    return hprof_data.lookup_instances_of_class("java.lang.String")


def app_interned_string_instances(hprof_data):
    return [
        root.obj
        for root in hprof_data.roots
        if root.heap_tag is HeapTag.ROOT_INTERNED_STRING
        and root.obj.heap.name != "zygote"
    ]


def app_non_interned_string_instances(hprof_data):
    interned = set(app_interned_string_instances(hprof_data))
    return [
        s
        for s in hprof_data.lookup_instances_of_class("java.lang.String")
        if s not in interned and s.heap.name != "zygote"
    ]


def app_roots(hprof_data):
    return [root for root in hprof_data.roots if root.obj.heap.name != "zygote"]


def roots_of_obj(hprof_data, obj):
    roots = []
    if obj.is_root:
        roots.append(obj)

    visited = set()
    references = hprof_data.lookup_references(obj)
    while len(references) > 0:
        new_references = []
        for reference in references:
            if reference.referer not in visited:
                visited.add(reference.referer)
                new_references.append(reference)
                if reference.referer.is_root:
                    roots.append(reference.referer)
        references = new_references
    return roots


def zygote_references_to_app_objects(hprof_data):
    references = []
    for obj in hprof_data.object_id_dict.values():
        if obj.heap.name == "zygote":
            for reference in obj.outgoing_references():
                if reference.referee.heap.name != "zygote":
                    references.append(reference)
    return references


def bitmap_instances(hprof_data):
    return hprof_data.lookup_instances_of_class("android.graphics.Bitmap")


def app_bitmap_instances(hprof_data):
    return [x for x in bitmap_instances(hprof_data) if x.heap.name != "zygote"]


def write_bitmap(bitmap_instance, filename):
    # Need to install PIL
    # sudo pip install pillow
    from PIL import Image

    bitmap_bytes = bitmap_instance.fields.mBuffer.array_data
    image = Image.frombytes(
        "RGBA",
        (bitmap_instance.fields.mWidth, bitmap_instance.fields.mHeight),
        bitmap_bytes,
    )
    image.save(filename)


def open_bitmaps(bitmap_instances):
    tmp_dir = tempfile.mkdtemp(suffix="bitmaps")
    subprocess.call(["open", tmp_dir])  # this only works in Mac - sorry!
    print("Writing %d bitmaps to %s." % (len(bitmap_instances), tmp_dir))
    for i, bitmap in enumerate(bitmap_instances):
        write_bitmap(bitmap, os.path.join(tmp_dir, "bitmap_%s.png" % bitmap.object_id))
        sys.stdout.write("\r%d of %d complete" % (i + 1, len(bitmap_instances)))
        sys.stdout.flush()
    print("")  # terminate line


def view_roots(hprof_data):
    return hprof_data.lookup_instances_of_class("android.view.ViewRootImpl")


def print_view_tree(view_root=None):
    if isinstance(view_root, HprofData):
        all_view_roots = view_roots(view_root)
        if len(all_view_roots) != 1:
            raise Exception("Please specify view root explicitly: %s" % all_view_roots)
        else:
            view_root = all_view_roots[0]
    else:
        print("not an hprofdata: %s" % view_root.__class__)

    print("%s" % view_root)

    def print_view_node(view_node, indent):
        print("%s%s" % (indent, view_node))
        if "android.view.ViewGroup" in view_node.class_fields:
            children = view_node.class_fields["android.view.ViewGroup"]["mChildren"]
            for child in children.array_values:
                if child is not None:
                    print_view_node(child, indent + "  ")

    print_view_node(view_root.fields.mView, "  ")


def reachable(instance, filter_function=lambda x: True):
    if isinstance(instance, (list, set)):
        instances = instance
        if len(instances) == 0:
            return []
    else:
        instances = [instance]

    seen = set(instances)
    reachable = set(instances)
    referees = list(reachable)
    while len(referees) > 0:
        new_referees = []
        for referee in referees:
            for reference in referee.outgoing_references():
                if reference.referee not in seen:
                    if filter_function(reference.referee):
                        reachable.add(reference.referee)
                        new_referees.append(reference.referee)
                    seen.add(reference.referee)
        referees = new_referees
    return reachable


def reachable_size(instance):
    return sum(x.shallow_size() for x in reachable(instance))


def retained(instance):
    if isinstance(instance, (list, set)):
        instances = instance
        if len(instances) == 0:
            return []
    else:
        instances = [instance]

    reachable_set = reachable(instances)
    return retained_in_set(instances, reachable_set)


def retained_size(instance):
    return sum(x.shallow_size() for x in retained(instance))


def retained_in_set(instances, reachable_set):
    if isinstance(instances, (list, set)):
        if instances:
            hprof_data = next(iter(instances)).hprof_data
        else:
            return []
    else:
        hprof_data = instances.hprof_data

    # keep the initial set around so nothing from that ever gets deleted
    initial_set = set(instances)
    # the set of instances on which to compute the retained set
    retained_set = set(instances)
    # objects under investigation, usually a breadth-first walk
    current = set(instances)
    # the subset of reachable_set that has incoming references outside of reachable_set
    escaped = set()

    # walk the incoming references to the given object returning true if
    # there is a reference outside the reachable_set set
    def reference_escapes(obj):
        visited = set()
        objects = [obj]
        while len(objects) > 0:
            new_objects = []
            for o in objects:
                if o in retained_set:
                    continue  # by definition
                if o in escaped:
                    # 'in escaped' is equivalent to 'not in reachable_set'
                    # because to be in the escaped set means that (transitively) a
                    # reference was 'not in reachable_set'
                    return True
                if o.is_root:
                    # a gc root is not in the retained set by definition
                    # unless it was part of the initial set but that was
                    # already accounted for above
                    return True
                for reference in hprof_data.lookup_references(o):
                    referer = reference.referer
                    if referer not in visited:
                        if referer not in reachable_set or referer in escaped:
                            return True
                        visited.add(referer)
                        new_objects.append(referer)
            objects = new_objects
        return False

    # walk the outgoing references of an object adding all elements to
    # the escaped set. Notice: given that the object in input is in the reachable_set
    # set all outgoing references must be in the reachable_set set.
    # Effectively this operation trims the reachable_set set and possibly
    # the current retained set
    def remove_escaped_ref(obj):
        current = set()
        if obj not in escaped and obj not in initial_set:
            escaped.add(obj)
            retained_set.discard(obj)
            current.add(obj)
        while len(current) > 0:
            new_current = set()
            for reference in obj.outgoing_references():
                ref = reference.referee
                if ref not in escaped and ref not in initial_set:
                    escaped.add(ref)
                    retained_set.discard(ref)
                    new_current.add(ref)
            current = new_current

    # walk the graph from the set of roots down, and for every object check
    # if it has incoming references outside of the reachable_set set. If so the object
    # and all its outgoing references are *not* in the retained set
    while len(current) > 0:
        new_current = set()
        for obj in current:
            if reference_escapes(obj):
                remove_escaped_ref(obj)
                continue
            # add it to the retained set, check the children
            retained_set.add(obj)
            for reference in obj.outgoing_references():
                ref = reference.referee
                if (
                    ref not in escaped
                    and ref not in retained_set
                    and ref in reachable_set
                ):
                    new_current.add(ref)
        current = new_current

    return retained_set


def wasted_segments(char_array):
    strings = [ref.referer for ref in char_array.incoming_references()]
    for s in strings:
        if not isinstance(s, HprofString):
            # Don't know anything about non-string references
            return []

    if len(strings) == 0:
        # It's garbage-collectible, so don't count it as wasted
        return []

    def forward_comparator(x, y):
        if x.fields.offset != y.fields.offset:
            return x.fields.offset - y.fields.offset
        else:
            return x.fields.count - y.fields.count

    sorted_forward = sorted(strings, forward_comparator)

    segments = []
    current_segment_start = 0
    for s in sorted_forward:
        if s.fields.offset > current_segment_start:
            segments.append((current_segment_start, s.fields.offset))
        current_segment_start = max(
            current_segment_start, s.fields.offset + s.fields.count
        )

    # Look at remaining
    if char_array.num_elements > current_segment_start:
        segments.append((current_segment_start, char_array.num_elements))

    return segments


# substring can result in wasted char arrays
# This isn't exact - need to figure out way of determining unused chars in the middle
def wasted_string_char_arrays(hprof_data):
    char_arrays = filter(
        lambda v: isinstance(v, HprofPrimitiveArray) and v.prim_type is HprofBasic.CHAR,
        hprof_data.object_id_dict.values(),
    )
    with_wasted = map(lambda x: (x, wasted_segments(x)), char_arrays)
    return filter(lambda x: len(x[1]) > 0, with_wasted)


def wasted_string_char_count(hprof_data):
    wasted_char_array_info = wasted_string_char_arrays(hprof_data)

    def segment_length(segments):
        return sum(map(lambda x: x[1] - x[0], segments))

    return sum(map(lambda x: segment_length(x[1]), wasted_char_array_info))


def app_heap_objects(hprof_data):
    return [o for o in hprof_data.object_id_dict.values() if o.heap.name != "zygote"]


# return a set of containing 'clazz' and all its subclasses
def subclasses_of(hprof_data, clazz):
    classes = {clazz}
    children = clazz.children
    while len(children) > 0:
        classes = classes.union(children)
        new_children = []
        for child in children:
            new_children.extend(child.children)
        children = new_children
    return classes


# return all instances of the given class and all subclasses.
def instances_of(hprof_data, clazz):
    return instances_in(hprof_data, subclasses_of(hprof_data, clazz))


# return a set of all instances from a sequence of classes
def instances_in(hprof_data, classes):
    if not isinstance(classes, set):
        if isinstance(classes, list):
            classes = set(classes)
        else:
            classes = {classes}
    return {obj for obj in hprof_data.object_id_dict.values() if obj.clazz in classes}


# return a map of class => {instances} for the given sequence of instances
def group_by_class(instances):
    by_class = {}
    for obj in instances:
        if obj.clazz not in by_class:
            by_class[obj.clazz] = set()
        by_class[obj.clazz].add(obj)
    return by_class


# return a map from thread to locals for every thread
def java_locals(hprof_data):
    locs = [
        root
        for root in hprof_data.roots
        if root.heap_tag == HeapTag.ROOT_JAVA_FRAME
        or root.heap_tag == HeapTag.ROOT_JNI_LOCAL
    ]
    threads = {
        root.thread_serial: root.obj
        for root in hprof_data.roots
        if root.heap_tag == HeapTag.ROOT_THREAD_OBJECT
    }
    thread_locals = {thread: set() for thread in threads.values()}
    for loc in locs:
        thread_locals[threads[loc.thread_serial]].add(loc.obj)
    return thread_locals


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--hprof", help="heap dump to generate class list from", required=True
    )
    parser.add_argument(
        "--allow_missing_ids",
        help="Unresolvable IDs result in only warnings, not errors",
        action="store_true",
    )

    args = parser.parse_args()

    allow_missing_ids = args.allow_missing_ids
    hp = parse_filename(args.hprof)
    classes = []
    for cls_name, cls in hp.class_name_dict.items():
        classes.append(
            (
                cls_name,
                hp.class_object_id_to_load_class_record[cls.object_id].class_serial,
            )
        )
    seen = set()
    class_serials = []
    for tup in classes:
        if tup[0] not in seen:
            seen.add(tup[0])
            if not tup[0].endswith("[]"):
                class_serials.append(tup)

    # On Dalvik these serial numbers correspond to classload order,
    # so it's useful to sort by them.
    class_serials.sort(key=lambda x: x[1])
    for cls in class_serials:
        class_name = str(cls[0]).replace(".", "/") + ".class"
        print(class_name)
