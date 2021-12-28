#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import bisect
import copy
import enum
import io
import numbers
import operator
import optparse
import os
import re
import string
import sys
import zipfile
from io import BytesIO

import file_extract
from file_extract import AutoParser


def get_uleb128_byte_size(value):
    byte_size = 1
    while value >= 0x80:
        byte_size += 1
        value >>= 7
    return byte_size


def get_uleb128p1_byte_size(value):
    return get_uleb128_byte_size(value + 1)


# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------
MAGIC = b"dex\n"
ENDIAN_CONSTANT = 0x12345678
REVERSE_ENDIAN_CONSTANT = 0x78563412
NO_INDEX = 0xFFFFFFFF
INT4_MIN = -8
INT4_MAX = 7
INT8_MIN = -128
INT8_MAX = 127
INT16_MIN = -32768
INT16_MAX = 32767
INT24_MIN = -8388608
INT24_MAX = 8388607
INT32_MIN = -2147483648
INT32_MAX = 2147483647
UINT4_MAX = 15
UINT8_MAX = 255
UINT16_MAX = 65535
UINT32_MAX = 4294967295


class AccessFlags(enum.IntFlag):
    PUBLIC = 0x1
    PRIVATE = 0x2
    PROTECTED = 0x4
    STATIC = 0x8
    FINAL = 0x10
    SYNCHRONIZED = 0x20
    VOLATILE = 0x40
    BRIDGE = 0x40
    TRANSIENT = 0x80
    VARARGS = 0x80
    NATIVE = 0x100
    INTERFACE = 0x200
    ABSTRACT = 0x400
    STRICT = 0x800
    SYNTHETIC = 0x1000
    ANNOTATION = 0x2000
    ENUM = 0x4000
    CONSTRUCTOR = 0x10000
    DECLARED_SYNCHRONIZED = 0x20000

    @classmethod
    def _missing_(cls, value):
        if isinstance(value, file_extract.FileExtract):
            return cls(value.get_uint8())
        else:
            return super()._missing_(value)

    def __str__(self):
        return ", ".join(flag.name for flag in AccessFlags if flag & self.value)


class ValueFormat(enum.Enum):
    BYTE = 0x00
    SHORT = 0x02
    CHAR = 0x03
    INT = 0x04
    LONG = 0x06
    FLOAT = 0x10
    DOUBLE = 0x11
    METHOD_TYPE = 0x15
    METHOD_HANDLE = 0x16
    STRING = 0x17
    TYPE = 0x18
    FIELD = 0x19
    METHOD = 0x1A
    ENUM = 0x1B
    ARRAY = 0x1C
    ANNOTATION = 0x1D
    NULL = 0x1E
    BOOLEAN = 0x1F

    @classmethod
    def _missing_(cls, value):
        if isinstance(value, file_extract.FileExtract):
            return cls(value.get_uint16())
        else:
            return super()._missing_(value)


class TypeCode(enum.Enum):
    HEADER_ITEM = 0x0000
    STRING_ID_ITEM = 0x0001
    TYPE_ID_ITEM = 0x0002
    PROTO_ID_ITEM = 0x0003
    FIELD_ID_ITEM = 0x0004
    METHOD_ID_ITEM = 0x0005
    CLASS_DEF_ITEM = 0x0006
    CALL_SITE_ID_ITEM = 0x0007
    METHOD_HANDLE_ITEM = 0x0008
    MAP_LIST = 0x1000
    TYPE_LIST = 0x1001
    ANNOTATION_SET_REF_LIST = 0x1002
    ANNOTATION_SET_ITEM = 0x1003
    CLASS_DATA_ITEM = 0x2000
    CODE_ITEM = 0x2001
    STRING_DATA_ITEM = 0x2002
    DEBUG_INFO_ITEM = 0x2003
    ANNOTATION_ITEM = 0x2004
    ENCODED_ARRAY_ITEM = 0x2005
    ANNOTATIONS_DIRECTORY_ITEM = 0x2006

    @classmethod
    def _missing_(cls, value):
        if isinstance(value, file_extract.FileExtract):
            return cls(value.get_uint16())
        else:
            return super()._missing_(value)

    def dump(self, prefix=None, f=sys.stdout, print_name=True, parent_path=None):
        f.write(self.name)

    @staticmethod
    def max_width():
        return max(len(bin(flag.value)) for flag in TypeCode)


class MethodHandleTypeCode(enum.Enum):
    STATIC_PUT = 0x00
    STATIC_GET = 0x01
    INSTANCE_PUT = 0x02
    INSTANCE_GET = 0x03
    INVOKE_STATIC = 0x04
    INVOKE_INSTANCE = 0x05

    @classmethod
    def _missing_(cls, value):
        if isinstance(value, file_extract.FileExtract):
            return cls(value.get_uint16())
        else:
            return super()._missing_(value)


PRINTABLE = string.ascii_letters + string.digits + string.punctuation + " "


def escape(c):
    global PRINTABLE
    if c in PRINTABLE:
        return c
    c = ord(c)
    if c <= 0xFF:
        return "\\x" + "%02.2x" % (c)
    elif c <= "\uffff":
        return "\\u" + "%04.4x" % (c)
    else:
        return "\\U" + "%08.8x" % (c)


def print_string(s, f):
    f.write('"')
    f.write("".join(escape(c) for c in s))
    f.write('"')


def print_version(version, f):
    if len(version) == 3:
        f.write("%u.%u.%u\n" % (version[0], version[1], version[2]))


def print_hex_bytes(data, f):
    for byte in data:
        f.write("%2.2x" % (byte))
    f.write("\n")


def print_endian(value, f):
    f.write("%#8.8x" % (value))
    if value == ENDIAN_CONSTANT:
        f.write(" (ENDIAN_CONSTANT)")
    elif value == REVERSE_ENDIAN_CONSTANT:
        f.write(" (REVERSE_ENDIAN_CONSTANT)")
    f.write("\n")


def is_zero(value):
    if value == 0:
        return None
    return "value should be zero, bit is %s" % (str(value))


def is_dex_magic(magic):
    if magic == MAGIC:
        return None
    return "value should be %s but is %s" % (MAGIC, magic)


def hex_escape(s):
    return "".join(escape(c) for c in s)


# ----------------------------------------------------------------------
# encoded_field
# ----------------------------------------------------------------------
class encoded_field(AutoParser):
    items = [
        {"type": "uleb", "name": "field_idx", "format": "%u"},
        {"type": "uleb", "name": "access_flags", "format": "0x%8.8x"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    @classmethod
    def fixup_indexes(cls, items):
        for i in range(1, len(items)):
            items[i].field_idx += items[i - 1].field_idx

    @classmethod
    def get_table_header(cls):
        return "FIELD FLAGS\n"

    def get_dump_flat(self):
        return True

    def get_access_flags(self):
        return AccessFlags(self.access_flags)


# ----------------------------------------------------------------------
# encoded_method
# ----------------------------------------------------------------------


class encoded_method(AutoParser):
    items = [
        {"type": "uleb", "name": "method_idx", "format": "%u"},
        {"type": "uleb", "name": "access_flags", "format": "0x%8.8x"},
        {"type": "uleb", "name": "code_off", "format": "0x%8.8x"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    @classmethod
    def fixup_indexes(cls, items):
        for i in range(1, len(items)):
            items[i].method_idx += items[i - 1].method_idx

    @classmethod
    def get_table_header(cls):
        return "METHOD FLAGS\n"

    def get_dump_flat(self):
        return True

    def get_access_flags(self):
        return AccessFlags(self.access_flags)


# ----------------------------------------------------------------------
# class_data_item
# ----------------------------------------------------------------------


class class_data_item(AutoParser):
    items = [
        {"type": "uleb", "name": "static_fields_size"},
        {"type": "uleb", "name": "instance_fields_size"},
        {"type": "uleb", "name": "direct_methods_size"},
        {"type": "uleb", "name": "virtual_methods_size"},
        {
            "class": encoded_field,
            "name": "static_fields",
            "attr_count": "static_fields_size",
            "flat": True,
        },
        {
            "class": encoded_field,
            "name": "instance_fields",
            "attr_count": "instance_fields_size",
            "flat": True,
        },
        {
            "class": encoded_method,
            "name": "direct_methods",
            "attr_count": "direct_methods_size",
            "flat": True,
        },
        {
            "class": encoded_method,
            "name": "virtual_methods",
            "attr_count": "virtual_methods_size",
            "flat": True,
        },
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)
        encoded_field.fixup_indexes(self.static_fields)
        encoded_field.fixup_indexes(self.instance_fields)
        encoded_method.fixup_indexes(self.direct_methods)
        encoded_method.fixup_indexes(self.virtual_methods)

    @classmethod
    def create_empty(cls):
        data = file_extract.FileExtract(BytesIO(b"\0\0\0\0"), "=")
        return class_data_item(data)


# ----------------------------------------------------------------------
# class_def_item
# ----------------------------------------------------------------------


class class_def_item(AutoParser):
    items = [
        {"type": "u32", "name": "class_idx", "align": 4},
        {"type": "u32", "name": "access_flags"},
        {"type": "u32", "name": "superclass_idx"},
        {"type": "u32", "name": "interfaces_off"},
        {"type": "u32", "name": "source_file_idx"},
        {"type": "u32", "name": "annotations_off"},
        {"type": "u32", "name": "class_data_off"},
        {"type": "u32", "name": "static_values_off"},
        {
            "class": class_data_item,
            "name": "class_data",
            "attr_offset": "class_data_off",
            "condition": lambda item, data: item.class_data_off != 0,
            "dump": False,
            "default": class_data_item.create_empty(),
        },
    ]

    def __init__(self, data, context):
        AutoParser.__init__(self, self.items, data, context)
        self.data = data
        self.interface_ids = None

    @classmethod
    def get_table_header(cls):
        return (
            "CLASS      ACCESS     SUPERCLASS INTERFACES SOURCE"
            "     ANNOTATION CLASS_DATA STATIC_VALUES\n"
        )

    def get_dump_flat(self):
        return True

    def find_encoded_method_by_code_off(self, code_off):
        for encoded_method in self.class_data.direct_methods:
            if encoded_method.code_off == code_off:
                return encoded_method
        for encoded_method in self.class_data.virtual_methods:
            if encoded_method.code_off == code_off:
                return encoded_method
        return None

    def get_interface_ids(self):
        if self.interface_ids is not None:
            return self.interface_ids
        elif self.interfaces_off > 0:
            self.data.push_offset_and_seek(self.interfaces_off)
            self.interface_ids = type_list(self.data).list
            self.data.pop_offset_and_seek()
        else:
            self.interface_ids = []
        return self.interface_ids

    def get_access_flags(self):
        return AccessFlags(self.access_flags)


# ----------------------------------------------------------------------
# try_item
# ----------------------------------------------------------------------


class try_item(AutoParser):
    items = [
        {"type": "u32", "name": "start_addr"},
        {"type": "u16", "name": "insn_count"},
        {"type": "u16", "name": "handler_off"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# encoded_type_addr_pair
# ----------------------------------------------------------------------
class encoded_type_addr_pair(AutoParser):
    items = [
        {"type": "uleb", "name": "type_idx", "format": "%#8.8x"},
        {"type": "uleb", "name": "addr", "format": "%#8.8x"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# encoded_catch_handler
# ----------------------------------------------------------------------


class encoded_catch_handler(AutoParser):
    items = [
        {"type": "sleb", "name": "size"},
        {
            "class": encoded_type_addr_pair,
            "name": "handlers",
            "attr_count": "size",
            "attr_count_fixup": abs,
        },
        {
            "type": "uleb",
            "name": "catch_all_addr",
            "default": 0,
            "condition": lambda item, data: item.size <= 0,
        },
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# encoded_catch_handler_list
# ----------------------------------------------------------------------


class encoded_catch_handler_list(AutoParser):
    items = [
        {"type": "uleb", "name": "size"},
        {"class": encoded_catch_handler, "name": "list", "attr_count": "size"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_flat(self):
        return True


def print_instructions(insns, prefix, flat, f, context=None):
    f.write("\n")
    code_units = CodeUnits(insns)
    dex_inst = DexInstruction()
    while code_units.index_is_valid():
        dex_inst.decode(code_units)
        if prefix:
            f.write(prefix)
        f.write("    ")
        dex_inst.dump(context=context)


class DBG(enum.IntEnum):
    END_SEQUENCE = 0x00
    ADVANCE_PC = 0x01
    ADVANCE_LINE = 0x02
    START_LOCAL = 0x03
    START_LOCAL_EXTENDED = 0x04
    END_LOCAL = 0x05
    RESTART_LOCAL = 0x06
    SET_PROLOGUE_END = 0x07
    SET_EPILOGUE_BEGIN = 0x08
    SET_FILE = 0x09

    @staticmethod
    def is_special_opcode(value):
        return value >= 0x0A and value <= 0xFF

    @classmethod
    def _missing_(cls, value):
        val = value
        if isinstance(value, file_extract.FileExtract):
            val = value.get_uint8()
        try:
            return cls(val)
        except ValueError:
            return super()._missing_(value)

    def dump(self, prefix=None, f=sys.stdout, print_name=True, parent_path=None):
        f.write(self.name)


def decode_DBG_or_val(value):
    val = value
    if isinstance(value, file_extract.FileExtract):
        val = value.get_uint8()
    return val if DBG.is_special_opcode(val) else DBG(val)


class debug_info_op(AutoParser):
    items = [
        {"decode": decode_DBG_or_val, "name": "op"},
        {
            "switch": "op",
            "cases": {
                DBG.ADVANCE_PC: [{"type": "uleb", "name": "addr_offset"}],
                DBG.ADVANCE_LINE: [{"type": "sleb", "name": "line_offset"}],
                DBG.START_LOCAL: [
                    {"type": "uleb", "name": "register_num"},
                    {"type": "ulebp1", "name": "name_idx"},
                    {"type": "ulebp1", "name": "type_idx"},
                ],
                DBG.START_LOCAL_EXTENDED: [
                    {"type": "uleb", "name": "register_num"},
                    {"type": "ulebp1", "name": "name_idx"},
                    {"type": "ulebp1", "name": "type_idx"},
                    {"type": "ulebp1", "name": "sig_idx"},
                ],
                DBG.END_LOCAL: [{"type": "uleb", "name": "register_num"}],
                DBG.RESTART_LOCAL: [{"type": "uleb", "name": "register_num"}],
                DBG.SET_FILE: [{"type": "ulebp1", "name": "name_idx"}],
                "default": [],
            },
        },
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)
        if DBG.is_special_opcode(self.op):
            line_base = -4
            line_range = 15
            adjusted_opcode = self.op - 0x0A
            self.line_offset = line_base + (adjusted_opcode % line_range)
            self.addr_offset = int(adjusted_opcode / line_range)
        self.byte_size = data.tell() - self.get_offset()

    def get_dump_flat(self):
        return True

    def get_byte_size(self):
        return self.byte_size

    def dump_opcode(self, f=sys.stdout):
        if isinstance(self.op, DBG):
            f.write(self.op.name)
        else:
            assert DBG.is_special_opcode(self.op)
            f.write(f"SPECIAL_OPCODE({self.op})")
        if self.op == DBG.ADVANCE_PC:
            f.write("(%u)" % self.addr_offset)
        elif self.op == DBG.ADVANCE_LINE:
            f.write("(%u)" % self.line_offset)
        elif self.op == DBG.START_LOCAL:
            f.write("(register_num=%u, name_idx=" % self.register_num)
            if self.name_idx < 0:
                f.write("NO_INDEX")
            else:
                f.write("%u" % (self.name_idx))
            f.write(", type_idx=")
            if self.type_idx < 0:
                f.write("NO_INDEX)")
            else:
                f.write("%u)" % (self.type_idx))
        elif self.op == DBG.START_LOCAL_EXTENDED:
            f.write("(register_num=%u, name_idx=" % self.register_num)
            if self.name_idx < 0:
                f.write("NO_INDEX")
            else:
                f.write("%u" % (self.name_idx))
            f.write(", type_idx=")
            if self.type_idx < 0:
                f.write("NO_INDEX")
            else:
                f.write("%u" % (self.type_idx))
            f.write(", sig_idx=")
            if self.type_idx < 0:
                f.write("NO_INDEX)")
            else:
                f.write("%u)" % (self.type_idx))
        elif self.op == DBG.END_LOCAL or self.op == DBG.RESTART_LOCAL:
            f.write("(register_num=%u)" % self.register_num)
        elif self.op == DBG.SET_FILE:
            f.write("(name_idx=%u)" % self.name_idx)
        elif DBG.is_special_opcode(self.op):
            f.write(
                " (addr_offset=%u, line_offset=%i)"
                % (self.addr_offset, self.line_offset)
            )


class debug_info_item(AutoParser):
    items = [
        {"type": "uleb", "name": "line_start"},
        {"type": "uleb", "name": "parameters_size"},
        {"type": "ulebp1", "name": "parameter_names", "attr_count": "parameters_size"},
    ]

    class row(object):
        def __init__(self):
            self.address = 0
            self.line = 0
            self.source_file = -1
            self.prologue_end = False
            self.epilogue_begin = False

        def dump(self, f=sys.stdout):
            f.write("0x%4.4x %5u %5u " % (self.address, self.line, self.source_file))
            if self.prologue_end or self.epilogue_begin:
                if self.prologue_end:
                    f.write("P ")
                else:
                    f.write("  ")
                if self.epilogue_begin:
                    f.write("E")
            f.write("\n")

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)
        self.data = data
        self.ops = None
        self.line_table = None
        self.debug_info_offset = data.tell()

    def check_encoding(self, dex_method, f=sys.stdout):
        bytes_saved = 0
        ops = self.get_ops()
        if len(ops) == 1:
            op = ops[0]
            if op.op == DBG.END_SEQUENCE:
                bytes_saved += get_uleb128_byte_size(
                    self.line_start
                ) + get_uleb128p1_byte_size(self.parameters_size)

                for parameter_name in self.parameter_names:
                    bytes_saved += get_uleb128p1_byte_size(parameter_name)
                bytes_saved += 1
                f.write(
                    "warning: %s debug info contains only a single "
                    % (dex_method.get_qualified_name())
                )
                f.write("%s, all debug info can be removed " % (op.op))
                f.write("(%u bytes)\n" % (bytes_saved))
                return bytes_saved
        # Dex files built for release don't need any the following
        # debug info ops
        for op in ops:
            size = op.get_byte_size()
            if op.op == DBG.SET_PROLOGUE_END:
                f.write(
                    "warning: %s %s can be removed (%u byte)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
            elif op.op == DBG.SET_EPILOGUE_BEGIN:
                f.write(
                    "warning: %s %s can be removed (%u byte)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
            elif op.op == DBG.START_LOCAL:
                f.write(
                    "warning: %s %s can be removed (%u bytes)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
            elif op.op == DBG.START_LOCAL_EXTENDED:
                f.write(
                    "warning: %s %s can be removed (%u bytes)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
            elif op.op == DBG.END_LOCAL:
                f.write(
                    "warning: %s %s can be removed (%u bytes)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
            elif op.op == DBG.RESTART_LOCAL:
                f.write(
                    "warning: %s %s can be removed (%u bytes)\n"
                    % (dex_method.get_qualified_name(), op.op, size)
                )
                bytes_saved += size
        return bytes_saved

    def get_line_table(self):
        if self.line_table is None:
            line_table = []
            ops = self.get_ops()
            row = debug_info_item.row()
            for op_args in ops:
                op = op_args.op
                if op == DBG.END_SEQUENCE:
                    break
                if op == DBG.ADVANCE_PC:
                    row.address += op_args.addr_offset
                elif op == DBG.ADVANCE_LINE:
                    row.line += op_args.line_offset
                elif op == DBG.START_LOCAL:
                    pass
                elif op == DBG.START_LOCAL_EXTENDED:
                    pass
                elif op == DBG.END_LOCAL:
                    pass
                elif op == DBG.RESTART_LOCAL:
                    pass
                elif op == DBG.SET_PROLOGUE_END:
                    row.prologue_end = True
                elif op == DBG.SET_EPILOGUE_BEGIN:
                    row.epilogue_begin = True
                elif op == DBG.SET_FILE:
                    row.source_file = op_args.name_idx
                else:
                    row.line += op_args.line_offset
                    row.address += op_args.addr_offset
                    line_table.append(copy.copy(row))
                    row.prologue_end = False
                    row.epilogue_begin = False
            self.line_table = line_table
        return self.line_table

    def get_ops(self, reset_offset=True):
        if self.ops is None:
            data = self.data
            if reset_offset:
                data.push_offset_and_seek(self.debug_info_offset)
            else:
                data.seek(self.debug_info_offset)
            self.ops = []
            while True:
                op = debug_info_op(data)
                self.ops.append(op)
                if op.op == DBG.END_SEQUENCE:
                    break
            if reset_offset:
                data.pop_offset_and_seek()
        return self.ops

    def dump_debug_info(self, f=sys.stdout, prefix=None, reset_offset=True):
        ops = self.get_ops(reset_offset=reset_offset)
        if prefix:
            f.write(prefix)
        f.write("    ")
        f.write(
            "line_start={}({}) param_size={}({}) param_name=[{}]\n".format(
                self.line_start,
                get_uleb128_byte_size(self.line_start),
                self.parameters_size,
                get_uleb128_byte_size(self.parameters_size),
                ", ".join(map(lambda x: str(x), self.parameter_names)),
            )
        )
        for op in ops:
            if prefix:
                f.write(prefix)
            f.write("    ")
            op.dump_opcode(f=f)
            f.write("\n")


# ----------------------------------------------------------------------
# code_item
# ----------------------------------------------------------------------
class code_item(AutoParser):
    items = [
        {"type": "u16", "name": "registers_size", "align": 4},
        {"type": "u16", "name": "ins_size"},
        {"type": "u16", "name": "outs_size"},
        {"type": "u16", "name": "tries_size"},
        {"type": "u32", "name": "debug_info_off"},
        {"type": "u32", "name": "insns_size", "format": "%u"},
        {
            "type": "u16",
            "name": "insns",
            "attr_count": "insns_size",
            "dump_list": print_instructions,
        },
        {
            "type": "u16",
            "condition": lambda item, data: item.tries_size != 0
            and item.insns_size & 1,
        },
        {
            "class": try_item,
            "name": "tries",
            "attr_count": "tries_size",
            "condition": lambda item, data: item.tries_size != 0,
            "default": None,
        },
        {
            "class": encoded_catch_handler_list,
            "name": "handlers",
            "condition": lambda item, data: item.tries_size != 0,
            "default": None,
        },
    ]

    def __init__(self, data, context):
        AutoParser.__init__(self, self.items, data, context)
        self.debug_info = None
        self.data = data
        # Convert insns from a list to a tuple to avoid mutation and also to
        # allow self.insns to be hashed.
        self.insns = tuple(self.insns)

    def get_debug_info(self):
        if self.debug_info is None and self.debug_info_off > 0:
            data = self.data
            data.push_offset_and_seek(self.debug_info_off)
            self.debug_info = debug_info_item(data)
            data.pop_offset_and_seek()
        return self.debug_info

    def dump(self, f, prefix, verbose=False):
        if verbose:
            AutoParser.dump(self, f=f, prefix=prefix)
        else:
            print_instructions(self.insns, prefix, False, f, self.context)


class encoded_value:
    def __init__(self, data):
        arg_type = data.get_uint8()
        value_arg = arg_type >> 5
        self.value_type = ValueFormat(arg_type & 0x1F)
        self.value = None
        size = value_arg + 1
        if self.value_type == ValueFormat.BYTE:
            if value_arg != 0:
                raise ValueError("VALUE_BYTE value_arg != 0 (%u)" % (value_arg))
            self.value = data.get_sint8()
        elif self.value_type == ValueFormat.SHORT:
            self.value = data.get_sint_size(size)
        elif self.value_type == ValueFormat.CHAR:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.INT:
            self.value = data.get_sint_size(size)
        elif self.value_type == ValueFormat.LONG:
            self.value = data.get_sint_size(size)
        elif self.value_type == ValueFormat.FLOAT:
            raise ValueError("VALUE_FLOAT not supported yet")
        elif self.value_type == ValueFormat.DOUBLE:
            raise ValueError("VALUE_DOUBLE not supported yet")
        elif self.value_type == ValueFormat.METHOD_TYPE:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.METHOD_HANDLE:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.STRING:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.TYPE:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.FIELD:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.METHOD:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.ENUM:
            self.value = data.get_uint_size(size)
        elif self.value_type == ValueFormat.ARRAY:
            if value_arg != 0:
                raise ValueError("VALUE_ARRAY value_arg != 0 (%u)" % (value_arg))
            raise ValueError("VALUE_ARRAY not supported yet")
            # encoded_array: an array of values, in the format specified by
            # "encoded_array format". The size of the value is implicit in
            # the encoding.
        elif self.value_type == ValueFormat.ANNOTATION:
            if value_arg != 0:
                raise ValueError("VALUE_ANNOTATION value_arg != 0 (%u)" % (value_arg))
            # encoded_annotation: a sub-annotation, in the format specified by
            # "encoded_annotation format" below. The size of the value is
            # implicit in the encoding.
        elif self.value_type == ValueFormat.NULL:
            if value_arg != 0:
                raise ValueError("VALUE_ARRAY value_arg != 0 (%u)" % (value_arg))
            self.value = 0
        elif self.value_type == ValueFormat.BOOLEAN:
            if size == 0:
                self.value = False
            else:
                self.value = data.get_uint8() != 0


# ----------------------------------------------------------------------
# encoded_array
# ----------------------------------------------------------------------


class encoded_array(AutoParser):
    items = [
        {"type": "uleb", "name": "size"},
        {"class": encoded_value, "name": "values", "attr_count": "size"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)


class encoded_array_item(AutoParser):
    items = [{"class": encoded_array, "name": "value"}]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)


# ----------------------------------------------------------------------
# field_id_item
# ----------------------------------------------------------------------


class field_id_item(AutoParser):
    items = [
        {"type": "u16", "name": "class_idx", "align": 4},
        {"type": "u16", "name": "type_idx"},
        {"type": "u32", "name": "name_idx"},
    ]

    def __init__(self, data, context):
        AutoParser.__init__(self, self.items, data, context)

    @classmethod
    def get_table_header(cls):
        return "CLASS  TYPE   NAME\n"

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# header_item
# ----------------------------------------------------------------------


class header_item(AutoParser):
    items = [
        {"type": "cstr[4]", "name": "magic", "validate": is_dex_magic},
        {"type": "u8[3]", "name": "version", "dump": print_version},
        {"type": "u8", "validate": is_zero},  # NULL byte
        {"type": "u32", "name": "checksum"},
        {"type": "u8[20]", "name": "signature", "dump": print_hex_bytes},
        {"type": "u32", "name": "file_size"},
        {"type": "u32", "name": "header_size"},
        {"type": "u32", "name": "endian_tag", "type": "u32", "dump": print_endian},
        {"type": "u32", "name": "link_size"},
        {"type": "u32", "name": "link_off"},
        {"type": "u32", "name": "map_off"},
        {"type": "u32", "name": "string_ids_size"},
        {"type": "u32", "name": "string_ids_off"},
        {"type": "u32", "name": "type_ids_size"},
        {"type": "u32", "name": "type_ids_off"},
        {"type": "u32", "name": "proto_ids_size"},
        {"type": "u32", "name": "proto_ids_off"},
        {"type": "u32", "name": "field_ids_size"},
        {"type": "u32", "name": "field_ids_off"},
        {"type": "u32", "name": "method_ids_size"},
        {"type": "u32", "name": "method_ids_off"},
        {"type": "u32", "name": "class_defs_size"},
        {"type": "u32", "name": "class_defs_off"},
        {"type": "u32", "name": "data_size"},
        {"type": "u32", "name": "data_off"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_header(self):
        return "DEX header:"


# ----------------------------------------------------------------------
# map_item
# ----------------------------------------------------------------------


class map_item(AutoParser):
    items = [
        {"class": TypeCode, "name": "type", "dump_width": TypeCode.max_width()},
        {"type": "u16"},
        {"type": "u32", "name": "size"},
        {"type": "u32", "name": "offset"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_list_header_lines(self):
        return ["      TYPE                            SIZE       OFFSET\n"]

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# map_list
# ----------------------------------------------------------------------


class map_list(AutoParser):
    items = [
        {"type": "u32", "name": "size", "align": 4, "dump": False},
        {"class": map_item, "name": "list", "attr_count": "size", "flat": True},
    ]

    def get_dump_header(self):
        return "map_list:"

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)


# ----------------------------------------------------------------------
# method_handle_item
# ----------------------------------------------------------------------


class method_handle_item(AutoParser):
    items = [
        {"class": MethodHandleTypeCode, "name": "method_handle_type", "align": 4},
        {"type": "u16"},
        {"type": "u16", "name": "field_or_method_id"},
        {"type": "u16"},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)


# ----------------------------------------------------------------------
# method_id_item
# ----------------------------------------------------------------------


class method_id_item(AutoParser):
    items = [
        {"type": "u16", "name": "class_idx", "align": 4},
        {"type": "u16", "name": "proto_idx"},
        {"type": "u32", "name": "name_idx"},
    ]

    def __init__(self, data, context):
        AutoParser.__init__(self, self.items, data, context)

    @classmethod
    def get_table_header(cls):
        return "CLASS  PROTO  NAME\n"

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# proto_id_item
# ----------------------------------------------------------------------


class proto_id_item(AutoParser):
    items = [
        {"type": "u32", "name": "shorty_idx", "align": 4},
        {"type": "u32", "name": "return_type_idx"},
        {"type": "u32", "name": "parameters_off"},
    ]

    def __init__(self, data, context):
        AutoParser.__init__(self, self.items, data, context)
        self.parameters = None

    def get_dump_flat(self):
        return True

    @classmethod
    def get_table_header(cls):
        return "SHORTY_IDX RETURN     PARAMETERS\n"

    def get_parameters(self):
        if self.parameters_off != 0 and self.parameters is None:
            # Get the data from our dex.File object
            data = self.context.data
            data.push_offset_and_seek(self.parameters_off)
            self.parameters = type_list(data)
            data.pop_offset_and_seek()
        return self.parameters


# ----------------------------------------------------------------------
# string_data_item
# ----------------------------------------------------------------------


class string_data_item(AutoParser):
    items = [
        {"type": "uleb", "name": "utf16_size", "format": "%3u"},
        {"type": "cstr", "name": "data", "dump": print_string},
    ]

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)

    def get_dump_flat(self):
        return True


# ----------------------------------------------------------------------
# type_list
# ----------------------------------------------------------------------
class type_list(AutoParser):
    items = [
        {"type": "u32", "name": "size", "align": 4},
        {"type": "u16", "name": "list", "attr_count": "size"},
    ]

    def get_dump_header(self):
        return "type_list:"

    def __init__(self, data):
        AutoParser.__init__(self, self.items, data)


class Progard:
    """Parses a proguard map file and does name lookups."""

    def __init__(self, path):
        self.path = path
        self.classes_dict = {}
        class_dict = None
        regex = re.compile(r"\s+([0-9]+:[0-9]+:)?(.*) -> (.*)$")
        with open(path, "r") as f:
            for line in f:
                line = line.rstrip("\n")
                if line:
                    if line[0].isspace():
                        match = regex.match(line)
                        if match:
                            old = match.group(2)
                            new = match.group(3)
                            # print('other old = "%s"' % (old))
                            # print('other new = "%s"' % (new))
                            class_dict[new] = old
                    else:
                        (old, new) = line.split(" -> ")
                        new = new.rstrip(":")
                        # print('class old = "%s"' % (old))
                        # print('class new = "%s"' % (new))
                        class_dict = {}
                        self.classes_dict[new] = (old, class_dict)

    def lookup_class(self, new_class):
        """Translate a new class name to the old class name."""
        if new_class in self.classes_dict:
            (old_class, class_dict) = self.classes_dict[new_class]
            if old_class is not None:
                return old_class
        return None

    def lookup_method(self, new_class, new_method):
        """Translate a new class name and a new method into the old class
        name and the old method name."""
        if new_class in self.classes_dict:
            (old_class, class_dict) = self.classes_dict[new_class]
            if new_method in class_dict:
                return class_dict[new_method]
        return None


class DexMethod:
    """Encapsulates a method within a DEX file."""

    def __init__(self, dex_class, encoded_method, is_virtual):
        self.dex_class = dex_class
        self.encoded_method = encoded_method
        self.method_id = None
        self.is_virtual = is_virtual
        self.code_item = None
        self.insns = None
        self.name_in_file = None
        self.name = None

    def __len__(self):
        ci = self.get_code_item()
        return (len(self.encoded_method) if self.encoded_method else 0) + (
            len(ci) if ci else 0
        )

    def get_signature(self):
        class_name = self.get_class().get_name()
        method_name = self.get_name()
        proto = self.get_pretty_proto()

        return class_name + "." + method_name + ":" + proto

    def get_qualified_name(self):
        class_name = self.get_class().get_name()
        method_name = self.get_name()
        if class_name[-1] != ";":
            return class_name + ":" + method_name
        else:
            return class_name + method_name

    def get_method_id(self):
        """Get the method_id_item for this method."""
        if self.method_id is None:
            self.method_id = self.get_dex().get_method_id(self.encoded_method)
        return self.method_id

    def get_method_index(self):
        """Get the method index into the method_ids array in the DEX file."""
        return self.encoded_method.method_idx

    def get_code_offset(self):
        """Get the code offset for this method."""
        return self.encoded_method.code_off

    def get_code_item_index(self):
        """Get the index into the code_items array in the dex file for the
        code for this method, or -1 if there is no code for this method."""
        code_item = self.get_code_item()
        if code_item:
            return self.get_dex().get_code_item_index_from_code_off(
                code_item.get_offset()
            )
        return -1

    def get_dex(self):
        return self.dex_class.get_dex()

    def get_name_in_file(self):
        """Returns the name of the method as it is known in the current DEX
        file (no proguard remapping)"""
        if self.name_in_file is None:
            self.name_in_file = self.get_dex().get_string(self.get_method_id().name_idx)
        return self.name_in_file

    def get_name(self):
        if self.name is None:
            cls_mangled = self.get_class().get_mangled_name()
            name_in_file = self.get_name_in_file()
            if cls_mangled and name_in_file:
                self.name = self.get_dex().demangle_class_method_name(
                    cls_mangled, name_in_file
                )
                if self.name is None:
                    self.name = name_in_file
        return self.name

    def get_internal_pretty_method_str_rep(self):
        # What it looks like boolean com.facebook.common.jit.common.JitDisabledChecker.testCompileMethod(int)
        dex = self.get_dex()
        proto_id = dex.get_proto_id(self.get_method_id().proto_idx)
        if proto_id is None:
            return None
        return_type = dex.get_typename(proto_id.return_type_idx)
        param = "("
        if proto_id.parameters_off != 0:
            # type is type_list
            param_list = proto_id.get_parameters().list
            not_first = False
            for type_idx in param_list:
                if not_first:
                    param += ", "
                param += dex.get_typename(type_idx)
        param += ")"

        cur_class_name = self.get_class().get_name()
        cur_method_name = self.get_name()

        return return_type + " " + cur_class_name + "." + cur_method_name + param

    def get_pretty_proto(self):
        dex = self.get_dex()
        proto_id = dex.get_proto_id(self.get_method_id().proto_idx)
        if proto_id is None:
            return None
        return_type = dex.get_raw_typename(proto_id.return_type_idx)
        param = "("
        if proto_id.parameters_off != 0:
            # type is type_list
            param_list = proto_id.get_parameters().list
            for type_idx in param_list:
                param += dex.get_raw_typename(type_idx)
        param += ")"
        return param + return_type

    def get_class(self):
        return self.dex_class

    def get_code_item(self):
        if self.code_item is None:
            if self.encoded_method.code_off != 0:
                self.code_item = self.get_dex().find_code_item(
                    self.encoded_method.code_off
                )
        return self.code_item

    def get_instruction_count(self):
        code_item = self.get_code_item()
        if code_item:
            return len(code_item.insns)
        return 0

    def get_code_byte_size(self):
        code_item = self.get_code_item()
        if code_item:
            return len(code_item.insns) * 2
        return 0

    def get_instructions(self):
        if self.insns is None:
            self.insns = []
            code_item = self.get_code_item()
            if code_item:
                code_units = CodeUnits(code_item.insns)
                while code_units.index_is_valid():
                    insn = DexInstruction()
                    insn.decode(code_units)
                    self.insns.append(insn)
        return self.insns

    def is_abstract(self):
        return bool(self.encoded_method.get_access_flags() & AccessFlags.ABSTRACT)

    def is_native(self):
        return bool(self.encoded_method.get_access_flags() & AccessFlags.NATIVE)

    def is_synthetic(self):
        return bool(self.encoded_method.get_access_flags() & AccessFlags.SYNTHETIC)

    def is_public(self):
        return bool(self.encoded_method.access_flags & AccessFlags.PUBLIC)

    def is_private(self):
        return bool(self.encoded_method.access_flags & AccessFlags.PRIVATE)

    def is_protected(self):
        return bool(self.encoded_method.access_flags & AccessFlags.PROTECTED)

    def dump(self, options, f=sys.stdout):
        dex = self.get_dex()
        method_id = dex.get_method_id(self.encoded_method.method_idx)
        f.write(
            "method: %s %s.%s:%s (%s)\n"
            % (
                "virtual" if self.is_virtual else "direct",
                self.get_class().get_name(),
                self.get_name(),
                dex.get_proto_string(method_id.proto_idx) if method_id else "",
                str(self.encoded_method.get_access_flags()),
            )
        )
        if options.verbose:
            self.encoded_method.dump(f=f, prefix="    encoded_method.", flat=False)
            if method_id:
                method_id.dump(f=f, prefix="    method_id.", flat=False)
                proto_id = dex.get_proto_id(method_id.proto_idx)
                if proto_id:
                    proto_id.dump(f=f, prefix="    proto_id.", flat=False)
            f.write("\n")
        code_item_idx = dex.get_code_item_index_from_code_off(
            self.encoded_method.code_off
        )
        if code_item_idx >= 0:
            code_item = dex.get_code_items()[code_item_idx]
            if options.verbose:
                f.write(
                    "    code_item[%u] @ %#8.8x:"
                    % (code_item_idx, code_item.get_offset())
                )
            code_item.dump(f=f, prefix="        ", verbose=options.verbose)
        if options.dump_debug_info:
            self.dump_debug_info(f=f, prefix="    ")

    def dump_code(self, f=sys.stdout):
        insns = self.get_instructions()
        for insn in insns:
            insn.dump(f=f)

    def get_debug_info(self):
        code_item = self.get_code_item()
        if code_item:
            return code_item.get_debug_info()
        return None

    def dump_debug_info(self, f=sys.stdout, prefix=None):
        debug_info = self.get_debug_info()
        if prefix:
            f.write(prefix)
        if debug_info:
            f.write("debug info @ %#8.8x:\n" % (debug_info.get_offset()))
            debug_info.dump_debug_info(f=f, prefix=prefix)
            f.write("\n")
        else:
            f.write("no debug info\n")

    def check_debug_info_encoding(self):
        debug_info = self.get_debug_info()
        if debug_info:
            return debug_info.check_encoding(self)

    def get_raw_access_flags(self):
        return str(self.encoded_method.access_flags)

    def get_line_number(self):
        debug_info = self.get_debug_info()
        if debug_info:
            return debug_info.line_start
        return 0


class DexClass:
    """Encapsulates a class within a DEX file."""

    def __init__(self, dex, class_def):
        self.dex = dex
        self.class_def = class_def
        self.methods = None
        self.unsorted_methods = None
        self.fields = None
        self.mangled = None
        self.demangled = None
        self.method_mapping = None

    def __len__(self):
        return sum((len(m) for m in self.get_methods(sort=False))) + sum(
            (len(f) for f in self.get_fields())
        )

    def dump(self, options, f=sys.stdout):
        dex = self.get_dex()
        class_def_offset = self.class_def.get_offset()
        class_def_idx = dex.get_class_def_index_from_offset(class_def_offset)
        class_data = self.class_def.class_data
        f.write("\nclass: %s // @%#8.8x" % (self.get_name(), class_def_offset))
        f.write(
            "\n\tAccess flags: (%s)\n\tSuperclass: %s\n\tInterfaces: (%s)"
            % (
                str(self.class_def.get_access_flags()),
                dex.get_typename(self.class_def.superclass_idx),
                ", ".join(
                    [
                        self.dex.get_typename(interface)
                        for interface in self.class_def.get_interface_ids()
                    ]
                ),
            )
        )
        field_ids = dex.get_field_ids()

        def field_to_string(field):
            field_item = field_ids[field.field_idx]
            return "(%s) %s:%s" % (
                str(field.get_access_flags()),
                dex.get_string(field_item.name_idx),
                dex.get_typename(field_item.type_idx),
            )

        f.write(
            "\n\tStatic fields:\n\t\t%s"
            % "\n\t\t".join(
                [
                    field_to_string(class_data.static_fields[i])
                    for i in range(class_data.static_fields_size)
                ]
            )
        )
        f.write(
            "\n\tInstance fields:\n\t\t%s\n"
            % "\n\t\t".join(
                [
                    field_to_string(class_data.instance_fields[i])
                    for i in range(class_data.instance_fields_size)
                ]
            )
        )
        if options.verbose:
            f.write("    class_def[%u] @ %#8.8x:\n" % (class_def_idx, class_def_offset))
            self.class_def.dump(f=f, flat=False, prefix="        ")
            f.write(
                "    class_data_item @ %#8.8x:\n"
                % (self.class_def.class_data.get_offset())
            )
            self.class_def.class_data.dump(f=f, flat=False, prefix="        ")
            f.write("\n")

    def get_type_index(self):
        """Get type ID index (class_idx) for this class."""
        return self.class_def.class_idx

    def is_abstract(self):
        return bool(self.class_def.get_access_flags() & AccessFlags.ABSTRACT)

    def is_public(self):
        return bool(self.class_def.access_flags & AccessFlags.PUBLIC)

    def is_private(self):
        return bool(self.class_def.access_flags & AccessFlags.PRIVATE)

    def is_protected(self):
        return bool(self.class_def.access_flags & AccessFlags.PROTECTED)

    def get_mangled_name(self):
        if self.mangled is None:
            dex = self.get_dex()
            self.mangled = dex.get_typename(self.class_def.class_idx)

        return self.mangled

    def get_name(self):
        """Get the demangled name for a class if we have a proguard file or
        return the mangled name if we don't have a proguard file."""
        if self.demangled is None:
            mangled = self.get_mangled_name()
            if mangled:
                self.demangled = self.get_dex().demangle_class_name(mangled)
                if self.demangled is None:
                    self.demangled = mangled
        return self.demangled

    def get_dex(self):
        return self.dex

    def get_methods(self, sort=True):
        if self.methods is not None:
            return self.methods
        if self.unsorted_methods is not None and not sort:
            return self.unsorted_methods

        if not sort:
            class_data = self.class_def.class_data
            self.unsorted_methods = [
                DexMethod(self, m, False) for m in class_data.direct_methods
            ]
            return self.unsorted_methods

        if self.unsorted_methods is not None:
            self.methods = sorted(
                self.unsorted_methods, key=lambda method: method.get_line_number()
            )
            self.unsorted_methods = None
        else:
            class_data = self.class_def.class_data
            self.methods = sorted(
                [DexMethod(self, m, False) for m in class_data.direct_methods]
                + [DexMethod(self, m, True) for m in class_data.virtual_methods],
                key=lambda method: method.get_line_number(),
            )
        return self.methods

    def get_fields(self):
        if self.fields is None:
            self.fields = []
            for encoded_field in self.class_def.class_data.static_fields:
                self.fields.append(DexField(self, encoded_field, False))
            for encoded_field in self.class_def.class_data.instance_fields:
                self.fields.append(DexField(self, encoded_field, True))

        return self.fields

    def get_super_cls_name(self):
        return self.get_dex().get_typename(self.class_def.superclass_idx)

    def get_method_mapping(self):
        if self.method_mapping is None:
            self.method_mapping = {}

            def insert_method(encoded_method, virtual):
                method = DexMethod(self, encoded_method, virtual)
                name = method.get_name_in_file()
                proto = method.get_pretty_proto()
                name_mapping = self.method_mapping.setdefault(name, {})
                if proto in name_mapping:
                    raise Exception(
                        "Unexpected duplicate method found: {}".format(method)
                    )
                name_mapping[proto] = method

            for encoded_method in self.class_def.class_data.direct_methods:
                insert_method(encoded_method, False)
            for encoded_method in self.class_def.class_data.virtual_methods:
                insert_method(encoded_method, True)
        return self.method_mapping

    def get_raw_access_flags(self):
        return str(self.class_def.access_flags)

    def find_method(self, method_name, proto):
        methods = self.get_method_mapping()
        if method_name not in methods:
            return None
        proto_map = methods[method_name]
        return proto_map.get(proto, None)

    def get_line_number(self):
        method_line_numbers = [
            method.get_line_number()
            for method in self.get_methods()
            if not method.is_abstract() and method.get_line_number() > 0
        ]
        if len(method_line_numbers) == 0:
            return 0
        return min(method_line_numbers)


class DexField:
    """Encapsulates a field within a DEX file."""

    def __init__(self, dex_class, encoded_field, is_instance_field):
        self.dex_class = dex_class
        self.encoded_field = encoded_field
        self.field_id = None
        self.name_in_file = None
        self.name = None
        self.is_instance_field = is_instance_field

    def __len__(self):
        return len(self.encoded_field) if self.encoded_field else 0

    def get_signature(self):
        class_name = self.get_class().get_name()
        field_name = self.get_name_in_file()
        field_type = self.get_type()

        return class_name + "." + field_name + ":" + field_type

    def get_type(self):
        return self.get_dex().get_typename(self.get_field_id().type_idx)

    def get_field_id(self):
        """Get the field_id for this field."""
        if self.field_id is None:
            self.field_id = self.get_dex().get_field_id(self.encoded_field.field_idx)
        return self.field_id

    def get_field_index(self):
        """Get the method index into the method_ids array in the DEX file."""
        return self.encoded_field.field_idx

    def get_dex(self):
        return self.dex_class.get_dex()

    def get_name_in_file(self):
        """Returns the name of the field as it is known in the current DEX
        file (no proguard remapping)"""
        if self.name_in_file is None:
            self.name_in_file = self.get_dex().get_string(self.get_field_id().name_idx)
        return self.name_in_file

    def get_name(self):
        if self.name is None:
            cls_mangled = self.get_class().get_mangled_name()
            name_in_file = self.get_name_in_file()
            if cls_mangled and name_in_file:
                self.name = self.get_dex().demangle_class_method_name(
                    cls_mangled, name_in_file
                )
                if self.name is None:
                    self.name = name_in_file
        return self.name

    def get_class(self):
        return self.dex_class

    def is_public(self):
        return bool(self.encoded_field.access_flags & AccessFlags.PUBLIC)

    def is_private(self):
        return bool(self.encoded_field.access_flags & AccessFlags.PRIVATE)

    def is_protected(self):
        return bool(self.encoded_field.access_flags & AccessFlags.PROTECTED)

    def get_raw_access_flags(self):
        return str(self.encoded_field.access_flags)


def demangle_classname(mangled):
    if mangled and len(mangled) > 2 and mangled[0] == "L" and mangled[-1] == ";":
        return mangled[1:-1].replace("/", ".") + ":"
    # Already demangled
    return mangled


def mangle_classname(demangled):
    if (
        demangled
        and len(demangled) > 2
        and (demangled[0] != "L" or demangled[-1] != ";")
    ):
        return "L" + demangled.replace(".", "/") + ";"
    # Already demangled
    return demangled


class File:
    """Represents a DEX (Dalvik Executable) file"""

    def __init__(
        self, path, file_like=None, proguard_path=None, use_bytecode_format=False
    ):
        self.path = path
        self.proguard = None
        if proguard_path and os.path.exists(proguard_path):
            self.proguard = Progard(proguard_path)
        if file_like is None:
            file_like = open(path, "rb")  # noqa: P201
        self.use_bytecode_format = use_bytecode_format
        self.data = file_extract.FileExtract(file_like, "=", 4)
        self.header = header_item(self.data)
        self.map_list = None
        self.string_ids = None
        self.type_ids = None
        self.proto_ids = None
        self.field_ids = None
        self.method_ids = None
        self.class_defs = None
        self.classes = None
        self.unsorted_classes = None
        self.call_site_ids = None
        self.method_handle_items = None
        self.code_items = None
        self.code_off_to_code_item_idx = {}
        self.strings = None
        self.call_sites = None
        self.dex_classes = {}
        self.debug_info_items = None
        self.debug_info_items_total_size = None

    def demangle_class_name(self, cls_mangled):
        """Given a mangled type name as it would appear in a DEX file like
        "LX/JxK;", return the demangled version if we have a proguard file,
        otherwise return the original class typename"""
        if self.proguard:
            cls_demangled = demangle_classname(cls_mangled)
            if cls_demangled:
                return self.proguard.lookup_class(cls_demangled)
        return None

    def demangle_class_method_name(self, cls_mangled, method_name):
        if self.proguard:
            cls_demangled = demangle_classname(cls_mangled)
            if cls_demangled:
                return self.proguard.lookup_method(cls_demangled, method_name)
        return None

    def get_map_list(self):
        if self.map_list is None:
            self.data.push_offset_and_seek(self.header.map_off)
            self.map_list = map_list(self.data)
            self.data.pop_offset_and_seek()
        return self.map_list

    def get_map_tuple(self, type_code):
        map_list = self.get_map_list()
        for item in map_list.list:
            if item.type == type_code:
                return (item.size, item.offset)
        return (0, 0)

    def get_debug_info_items_and_total_size(self):
        if self.debug_info_items is None:
            (size, offset) = self.get_map_tuple(TypeCode.DEBUG_INFO_ITEM)
            if size == 0 or offset == 0:
                return (None, None)
            self.data.push_offset_and_seek(offset)
            self.debug_info_items = []
            for _ in range(size):
                item = debug_info_item(self.data)
                item.get_ops(reset_offset=False)
                self.debug_info_items.append(item)
            self.debug_info_items_total_size = self.data.tell() - offset
            self.data.pop_offset_and_seek()
        return (self.debug_info_items, self.debug_info_items_total_size)

    def find_class(self, class_ref):
        class_idx = class_ref
        if isinstance(class_ref, str):
            # Make sure the string is in 'L' <classname-with-slashes> ';'
            class_mangled = mangle_classname(class_ref)
            class_str_idx = self.find_string_idx(class_mangled)
            if class_str_idx >= 0:
                class_idx = self.find_type_idx(class_str_idx)
        if isinstance(class_idx, numbers.Integral):
            classes = self.get_classes(sort=False)
            for cls in classes:
                if cls.class_def.class_idx == class_idx:
                    return cls
        return None

    def find_string_idx(self, match_s):
        match_key = match_s if isinstance(match_s, str) else match_s.data

        class StringsWrapper:
            def __init__(self, base):
                self._base = base

            def __getitem__(self, idx):
                return self._base[idx].data

            def __len__(self):
                return len(self._base)

        bisect_against = StringsWrapper(self.get_strings())

        i = bisect.bisect_left(bisect_against, match_key)
        if i != len(bisect_against) and bisect_against[i] == match_key:
            return i
        return -1

    def get_string(self, index):
        strings = self.get_strings()
        if index < len(strings):
            return file_extract.hex_escape(strings[index].data)
        return None

    def get_raw_typename(self, type_id):
        types = self.get_type_ids()
        if type_id < len(types):
            return self.get_string(types[type_id])
        return None

    def get_typename(self, type_id):
        raw_typename = self.get_raw_typename(type_id)
        if self.use_bytecode_format:
            return raw_typename

        if raw_typename is None:
            return None
        array_level = 0
        for c in raw_typename:
            if c == "[":
                array_level += 1
            else:
                break
        raw_prefix = raw_typename[array_level]
        raw_base_typename = raw_typename[array_level + 1 :]
        if raw_prefix == "L":
            assert raw_base_typename[-1] == ";"
            return raw_base_typename[:-1].replace("/", ".") + "[]" * array_level
        prefix_to_typename = {
            "V": "void",
            "Z": "boolean",
            "B": "byte",
            "S": "short",
            "C": "char",
            "I": "int",
            "J": "long",
            "F": "float",
            "D": "double",
        }
        return prefix_to_typename[raw_prefix] + "[]" * array_level

    def get_string_ids(self):
        if self.string_ids is None:
            self.string_ids = []
            self.data.push_offset_and_seek(self.header.string_ids_off)
            for _ in range(self.header.string_ids_size):
                self.string_ids.append(self.data.get_uint32())
            self.data.pop_offset_and_seek()
        return self.string_ids

    def get_type_ids(self):
        if self.type_ids is None:
            self.type_ids = []
            self.data.push_offset_and_seek(self.header.type_ids_off)
            for _ in range(self.header.type_ids_size):
                self.type_ids.append(self.data.get_uint32())
            self.data.pop_offset_and_seek()
        return self.type_ids

    def get_proto_ids(self):
        if self.proto_ids is None:
            self.proto_ids = []
            self.data.push_offset_and_seek(self.header.proto_ids_off)
            for _ in range(self.header.proto_ids_size):
                self.proto_ids.append(proto_id_item(self.data, self))
            self.data.pop_offset_and_seek()
        return self.proto_ids

    def get_proto_id(self, proto_idx):
        proto_ids = self.get_proto_ids()
        if proto_idx >= 0 and proto_idx < len(proto_ids):
            return proto_ids[proto_idx]
        return None

    def get_proto_shorty(self, proto_idx):
        id = self.get_proto_id(proto_idx)
        return self.get_string(id.shorty_idx)

    def get_proto_string(self, proto_idx):
        proto = self.get_proto_id(proto_idx)
        return_type = self.get_typename(proto.return_type_idx)
        params = proto.get_parameters()
        return "(%s)%s" % (
            ", ".join(
                [self.get_typename(type_idx) for type_idx in params.list]
                if params
                else []
            ),
            return_type,
        )

    def get_field_ids(self):
        if self.field_ids is None:
            self.field_ids = []
            self.data.push_offset_and_seek(self.header.field_ids_off)
            for _ in range(self.header.field_ids_size):
                self.field_ids.append(field_id_item(self.data, self))
            self.data.pop_offset_and_seek()
        return self.field_ids

    def get_field_id(self, field_ref):
        field_ids = self.get_field_ids()
        if field_ids:
            if isinstance(field_ref, encoded_field):
                if field_ref.field_idx < len(field_ids):
                    return field_ids[field_ref.field_id]
            elif isinstance(field_ref, numbers.Integral):
                if field_ref < len(field_ids):
                    return field_ids[field_ref]
            else:
                raise ValueError("invalid field_ref type %s" % (type(field_ref)))
        return None

    def get_method_ids(self):
        if self.method_ids is None:
            self.method_ids = []
            self.data.push_offset_and_seek(self.header.method_ids_off)
            for _ in range(self.header.method_ids_size):
                self.method_ids.append(method_id_item(self.data, self))
            self.data.pop_offset_and_seek()
        return self.method_ids

    def find_method_ids(self, method_name, class_ref=None):
        dex_class = None
        if class_ref is not None:
            dex_class = self.find_class(class_ref)
        matches = []  # Return a list of matching methods
        method_ids = self.get_method_ids()
        if not method_ids:
            return matches
        name_idx = self.find_string_idx(method_name)
        if name_idx <= 0:
            return matches
        for method_id in method_ids:
            if method_id.name_idx == name_idx:
                if dex_class:
                    if method_id.class_idx != dex_class.class_def.class_idx:
                        continue
                matches.append(method_id)
        return matches

    def find_method_id_by_code_offset(self, code_off):
        class_defs = self.get_class_defs()
        for class_def in class_defs:
            method_id = class_def.find_encoded_method_by_code_off(code_off)
            if method_id:
                return method_id
        return None

    def get_method_id(self, method_ref):
        """method_ref can be one of:
        - a encoded_method object
        - integer method index"""
        method_ids = self.get_method_ids()
        if method_ids:
            if isinstance(method_ref, encoded_method):
                if method_ref.method_idx < len(method_ids):
                    return method_ids[method_ref.method_idx]
            elif isinstance(method_ref, numbers.Integral):
                if method_ref < len(method_ids):
                    return method_ids[method_ref]
            else:
                raise ValueError("invalid method_ref type %s" % (type(method_ref)))
        return None

    # def get_call_site(self, idx):
    #     call_site_ids = self.get_call_site_ids()
    #     if idx >= len(call_site_ids):
    #         return None
    #     if self.call_sites[idx] is None:
    #         self.data.push_offset_and_seek(call_site_ids[idx])
    #         self.call_sites[idx] = call_site_item(self.data)
    #         self.data.pop_offset_and_seek()
    #     return self.call_sites[idx]

    def get_call_site_ids(self):
        if self.call_site_ids is None:
            self.call_site_ids = []
            self.call_sites = []
            (size, offset) = self.get_map_tuple(TypeCode.CALL_SITE_ID_ITEM)
            self.data.push_offset_and_seek(offset)
            for _ in range(size):
                self.call_site_ids.append(self.data.get_uint32())
                self.call_sites.append(None)
            self.data.pop_offset_and_seek()
        return self.call_site_ids

    def get_method_handle_items(self):
        if self.method_handle_items is None:
            self.method_handle_items = []
            (size, offset) = self.get_map_tuple(TypeCode.METHOD_HANDLE_ITEM)
            self.data.push_offset_and_seek(offset)
            for _ in range(size):
                self.method_handle_items.append(method_handle_item(self.data))
            self.data.pop_offset_and_seek()
        return self.method_handle_items

    def get_code_items(self):
        if self.code_items is None:
            self.code_items = []
            (size, offset) = self.get_map_tuple(TypeCode.CODE_ITEM)
            self.data.push_offset_and_seek(offset)
            for i in range(size):
                self.data.align_to(4)
                item = code_item(self.data, self)
                self.code_items.append(item)
                self.code_off_to_code_item_idx[item.get_offset()] = i
            self.data.pop_offset_and_seek()
        return self.code_items

    def report_code_duplication(self):
        code_to_code_items = {}
        code_items = self.get_code_items()
        if code_items:
            for code_item in code_items:
                key = code_item.insns
                if key in code_to_code_items:
                    code_to_code_items[key].append(code_item)
                else:
                    code_to_code_items[key] = [code_item]
            for key in code_to_code_items:
                code_items = code_to_code_items[key]
                if len(code_items) > 1:
                    print("-" * 72)
                    print("The following methods have the same code:")
                    for code_item in code_items:
                        method = self.find_method_from_code_off(code_item.get_offset())
                        if method.is_virtual:
                            print("virtual", end=" ")
                        else:
                            print("direct", end=" ")
                        print(method.get_qualified_name())
                    # Dump the code once for all methods
                    method.dump_code()

    def get_class_def_index_from_offset(self, class_def_offset):
        class_defs = self.get_class_defs()
        for (i, class_def) in enumerate(class_defs):
            if class_def.get_offset() == class_def_offset:
                return i
        return -1

    def get_code_item_index_from_code_off(self, code_off):
        # Make sure the code items are created
        self.get_code_items()
        if code_off in self.code_off_to_code_item_idx:
            return self.code_off_to_code_item_idx[code_off]
        return -1

    def find_code_item(self, code_off):
        code_item_idx = self.get_code_item_index_from_code_off(code_off)
        if code_item_idx >= 0:
            return self.get_code_items()[code_item_idx]
        else:
            raise ValueError("invalid code item offset %#8.8x" % code_off)

    def find_method_from_code_off(self, code_off):
        if code_off == 0:
            return None
        for cls in self.get_classes(sort=False):
            for method in cls.get_methods(sort=False):
                if method.get_code_offset() == code_off:
                    return method
        return None

    def get_class_defs(self):
        if self.class_defs is None:
            self.class_defs = []
            self.data.push_offset_and_seek(self.header.class_defs_off)
            for _ in range(self.header.class_defs_size):
                class_def = class_def_item(self.data, self)
                self.class_defs.append(class_def)
            self.data.pop_offset_and_seek()
        return self.class_defs

    def get_classes(self, sort=True):
        if self.classes is not None:
            return self.classes
        if self.unsorted_classes is not None and not sort:
            return self.unsorted_classes

        if not sort:
            self.unsorted_classes = [
                DexClass(self, class_def) for class_def in self.get_class_defs()
            ]
            return self.unsorted_classes

        if self.unsorted_classes is not None:
            self.classes = sorted(
                self.unsorted_classes, key=lambda cls: cls.get_line_number()
            )
            self.unsorted_classes = None
        else:
            self.classes = sorted(
                (DexClass(self, class_def) for class_def in self.get_class_defs()),
                key=lambda cls: cls.get_line_number(),
            )
        return self.classes

    def get_strings(self):
        if self.strings is None:
            self.strings = []
            for string_id_item in self.get_string_ids():
                self.data.push_offset_and_seek(string_id_item)
                self.strings.append(string_data_item(self.data))
            self.data.pop_offset_and_seek()
        return self.strings

    def dump_header(self, options, f=sys.stdout):
        self.header.dump(f=f)
        f.write("\n")

    def dump_map_list(self, options, f=sys.stdout):
        self.get_map_list().dump(f=f)
        f.write("\n")

    def dump_string_ids(self, options, f=sys.stdout):
        string_ids = self.get_string_ids()
        if string_ids:
            f.write("string_ids:\n")
            for (i, item) in enumerate(self.get_strings()):
                f.write("[%4u] %#8.8x ( " % (i, string_ids[i]))
                item.dump(f=f)
                f.write(")\n")

    def dump_type_ids(self, options, f=sys.stdout):
        type_ids = self.get_type_ids()
        if type_ids:
            f.write("\ntype_ids:\n      DESCRIPTOR_IDX\n")
            for (i, item) in enumerate(type_ids):
                f.write('[%4u] %#8.8x ("%s")\n' % (i, item, self.get_string(item)))

    def find_type_idx(self, class_str_idx):
        types = self.get_type_ids()
        i = bisect.bisect_left(types, class_str_idx)
        if i != len(types) and types[i] == class_str_idx:
            return i
        return -1

    def find_class_def_by_type_index(self, class_idx):
        class_defs = self.get_class_defs()
        for class_def in class_defs:
            if class_def.class_idx == class_idx:
                return class_def
        return None

    def dump_proto_ids(self, options, f=sys.stdout):
        proto_ids = self.get_proto_ids()
        if proto_ids:
            f.write("\nproto_ids:\n")
            f.write(" " * (6 + 1))
            f.write(proto_id_item.get_table_header())
            for (i, item) in enumerate(proto_ids):
                f.write("[%4u] " % (i))
                item.dump(f=f, print_name=False)
                f.write("%s\n" % self.get_proto_string(i))

    def dump_field_ids(self, options, f=sys.stdout):
        field_ids = self.get_field_ids()
        if field_ids:
            f.write("\nfield_ids:\n")
            f.write(" " * (6 + 1))
            f.write(field_id_item.get_table_header())
            for (i, item) in enumerate(field_ids):
                f.write("[%4u] " % (i))
                item.dump(f=f, print_name=False)
                f.write(
                    " %s.%s:%s\n"
                    % (
                        self.get_typename(item.class_idx),
                        self.get_string(item.name_idx),
                        self.get_typename(item.type_idx),
                    )
                )

    def dump_class_method_ids(self, options, f=sys.stdout):
        method_ids = self.get_method_ids()
        if not method_ids:
            return
        f.write("\nmethod_ids:\n")
        f.write(" " * (6 + 1))
        f.write(method_id_item.get_table_header())
        for dex_class in self.get_classes():
            f.write("\nclass %s\n" % dex_class.get_name())
            for dex_method in dex_class.get_methods():
                method_idx = dex_method.encoded_method.method_idx
                method_item = method_ids[method_idx]
                f.write("[%4u] " % method_idx)
                method_item.dump(f=f, print_name=False)
                f.write(
                    " %s.%s:%s (%s)\n"
                    % (
                        self.get_typename(method_item.class_idx),
                        self.get_string(method_item.name_idx),
                        self.get_proto_string(method_item.proto_idx),
                        str(dex_method.encoded_method.get_access_flags()),
                    )
                )

    def dump_class_defs(self, options, f=sys.stdout):
        class_defs = self.get_class_defs()
        if class_defs:
            f.write("\nclass_defs:\n")
            f.write(" " * (6 + 1))
            f.write(class_def_item.get_table_header())
            for (i, item) in enumerate(class_defs):
                f.write("[%4u] " % (i))
                item.dump(f=f, print_name=False)
                f.write(
                    "   (%s, %s, (%s))"
                    % (
                        self.get_typename(item.class_idx),
                        self.get_typename(item.superclass_idx),
                        str(item.get_access_flags()),
                    )
                )
                f.write("\n")

    def dump_call_site_ids(self, options, f=sys.stdout):
        call_site_ids = self.get_call_site_ids()
        if call_site_ids:
            f.write("\ncall_site_ids:\n")
            f.write(" " * (6 + 1))
            for (i, item) in enumerate(call_site_ids):
                f.write("[%4u] %#8.8x\n" % (i, item))

    def dump_method_handle_items(self, options, f=sys.stdout):
        method_handle_items = self.get_method_handle_items()
        if method_handle_items:
            f.write("\nmethod_handle_items:\n")
            f.write(" " * (6 + 1))
            for (i, item) in enumerate(method_handle_items):
                f.write("[%4u] " % (i))
                item.dump(f=f)
                f.write("\n")

    def dump_code(self, options, f=sys.stdout):
        classes = self.get_classes()
        if classes:
            for cls in classes:
                if options.skip_abstract and cls.is_abstract():
                    continue
                cls.dump(options, f=f)
                methods = cls.get_methods()
                if options.dump_code or options.dump_debug_info or options.dump_all:
                    for method in methods:
                        method.dump(options, f=f)
                f.write("\n")

    def dump_code_items(self, options, f=sys.stdout):
        code_items = self.get_code_items()
        if code_items:
            for (i, code_item) in enumerate(code_items):
                f.write("code_item[%u]:\n" % (i))
                code_item.dump(f=f)

    def dump_debug_info_items(self, options, f=sys.stdout):
        (debug_info_items, size) = self.get_debug_info_items_and_total_size()
        if debug_info_items:
            for item in debug_info_items:
                item.dump_debug_info(f=f)
            f.write("Total TYPE_DEBUG_INFO_ITEM size: {}\n\n".format(size))

    def dump_structure(self, options, f=sys.stdout):
        public_only = options.public_only
        classes = self.get_classes()
        for cls in classes:
            if public_only and not cls.is_public():
                continue

            methods = cls.get_methods()
            method_signature_to_access = {}
            for method in methods:
                if public_only and not method.is_public():
                    continue
                method_signature_to_access[
                    method.get_signature()
                ] = method.get_raw_access_flags()

            fields = cls.get_fields()
            field_signature_to_access = {}
            for field in fields:
                if public_only and not field.is_public():
                    continue
                field_signature_to_access[
                    field.get_signature()
                ] = field.get_raw_access_flags()

            f.write(
                "%s %s %s %d %d\n"
                % (
                    cls.get_name(),
                    cls.get_raw_access_flags(),
                    cls.get_super_cls_name(),
                    len(method_signature_to_access),
                    len(field_signature_to_access),
                )
            )

            for meth_signature, meth_access_flags in method_signature_to_access.items():
                f.write("    M %s %s\n" % (meth_signature, meth_access_flags))
            for (
                field_signature,
                field_access_flags,
            ) in field_signature_to_access.items():
                f.write("    F %s %s\n" % (field_signature, field_access_flags))

    def dump(self, options, f=sys.stdout):
        self.dump_header(options, f)
        f.write("\n")
        self.dump_map_list(options, f)
        self.dump_debug_info_items(options, f)
        self.dump_string_ids(options, f)
        self.dump_type_ids(options, f)
        self.dump_proto_ids(options, f)
        self.dump_field_ids(options, f)
        self.dump_method_ids(options, f)
        self.dump_class_defs(options, f)
        self.dump_call_site_ids(options, f)
        self.dump_method_handle_items(options, f)
        self.dump_code(options, f)
        self.dump_code_items(options, f)


def sign_extending(value, bit_width):
    # is the highest bit (sign) set? (x>>(b-1)) would be faster
    if value & (1 << (bit_width - 1)):
        return value - (1 << bit_width)  # 2s complement
    return value


def get_signed_hex_offset_as_str(signed_offset, width):
    if signed_offset < 0:
        s = "-"
        offset = abs(signed_offset)
    else:
        s = "+"
        offset = signed_offset
    if width == 2:
        s += "%2.2x" % (offset & 0xFF)
    elif width == 4:
        s += "%4.4x" % (offset & 0xFFFF)
    elif width == 8:
        s += "%8.8x" % (offset & 0xFFFFFFFF)
    else:
        raise ValueError("only sizes of 2 4 or 8 are supported")
    return s


class Opcode(object):
    def __init__(self, inst):
        self.inst = inst

    def check_encoding(self, f=sys.stdout):
        """Verify that this instruction can't be encoded more efficiently"""
        return 0  # Return zero to indicate we can't save any bytes

    def new_encoding(self, f=sys.stdout):
        """Look for bytes we can save by making new opcodes that are encoded
        as unsigned, or other optimizations"""
        return 0  # Return zero to indicate we can't save any bytes

    def get_op(self):
        return self.inst.get_op()

    def get_name(self):
        op = self.get_op()
        return self.ops[op]

    def get_num_code_units(self):
        return self.num_code_units

    def regs_are_sequential(self):
        if len(self.regs) <= 1:
            return True
        prev_reg = self.regs[0]
        for i in range(1, len(self.regs)):
            curr_reg = self.regs[i]
            if prev_reg + 1 != curr_reg:
                return False
        return True


class Opcode00(Opcode):
    ops = {0x00: "nop"}
    num_code_units = 1
    max_regs = 0
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.nature = inst.get_AA()
        if self.nature == 0:
            pass  # NOP
        elif self.nature == 1:
            self.size = code_units.get_code_unit()
            self.first_key = code_units.get_int()
            self.targets = []
            for _ in range(self.size):
                self.targets.append(code_units.get_int())
        elif self.nature == 2:
            self.size = code_units.get_code_unit()
            self.keys = []
            self.targets = []
            for _ in range(self.size):
                self.keys.append(code_units.get_int())
            for _ in range(self.size):
                self.targets.append(code_units.get_int())
        elif self.nature == 3:
            self.element_width = code_units.get_code_unit()
            self.size = code_units.get_uint()
            num_code_units = int((self.size * self.element_width + 1) / 2)
            encoder = file_extract.FileEncode(BytesIO(), "little", 4)
            for _ in range(num_code_units):
                encoder.put_uint16(code_units.get_code_unit())
            encoder.seek(0)
            self.data = encoder.file.getvalue()
        else:
            raise ValueError("add support for NOP nature %u" % (self.nature))

    def get_name(self):
        if self.nature == 0:
            return self.ops[0]
        elif self.nature == 1:
            return "packed-switch-payload"
        elif self.nature == 2:
            return "sparse-switch-payload"
        elif self.nature == 3:
            return "fill-array-data-payload"
        else:
            raise ValueError("add support for NOP nature %u" % (self.nature))

    def get_num_code_units(self):
        if self.nature == 0:
            return 1
        elif self.nature == 1:
            op_count = 1
            size_count = 1
            first_key_count = 2
            keys_count = self.size * 2
            return op_count + size_count + first_key_count + keys_count
        elif self.nature == 2:
            op_count = 1
            size_count = 1
            keys_and_targets_count = self.size * 4
            return op_count + size_count + keys_and_targets_count
        elif self.nature == 3:
            op_count = 1
            element_width_count = 2
            return op_count + element_width_count + len(self.data)
        else:
            raise ValueError("add support for NOP nature %u" % (self.nature))

    def dump(self, f=sys.stdout, context=None):
        if self.nature == 0:
            f.write("%s" % (self.get_name()))
        elif self.nature == 1:
            f.write("packed-switch-payload\n")
            f.write("INDEX  KEY       TARGET\n===== --------- ---------\n")
            for (i, target) in enumerate(self.targets):
                f.write("[%4u] %+8.8x %+8.8x\n" % (i, self.first_key + i, target))
        elif self.nature == 2:
            f.write("sparse-switch-payload\n")
            f.write("INDEX  KEY       TARGET\n===== --------- ---------\n")
            for (i, key) in enumerate(self.keys):
                f.write("[%4u] %+8.8x %+8.8x\n" % (i, key, self.targets[i]))
        elif self.nature == 3:
            f.write(
                "fill-array-data-payload (elem_width = %u, size = %u)\n"
                % (self.element_width, self.size)
            )
            file_extract.dump_memory(0, self.data, self.element_width, f)

    def emulate(self, emulator):
        pass


class Opcode01(Opcode):
    ops = {0x01: "move"}
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode02(Opcode):
    ops = {0x02: "move/from16"}
    num_code_units = 2
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst[1])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move/from16" can be encoded as a "move"')
            f.write(" more efficiently as its registers are both <= %u\n" % (UINT4_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode03(Opcode):
    ops = {0x03: "move/16"}
    num_code_units = 3
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst[1])
        self.regs.append(inst[2])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move/16" can be encoded as a "move"')
            f.write(" more efficiently as its registers are both <= %u\n" % (UINT4_MAX))
            return 4
        if self.regs[0] <= UINT8_MAX:
            f.write('warning: "move/16" can be encoded as a "move/from16"')
            f.write(" more efficiently as its first register is <= %u\n" % (UINT8_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode04(Opcode):
    ops = {0x04: "move-wide"}
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode05(Opcode):
    ops = {0x05: "move-wide/from16"}
    num_code_units = 2
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst[1])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move-wide/from16" can be encoded as a ')
            f.write('"move-wide" more efficiently as its registers are ')
            f.write("both <= %u\n" % (UINT4_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode06(Opcode):
    ops = {0x06: "move-wide/16"}
    num_code_units = 3
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst[1])
        self.regs.append(inst[2])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move-wide/16" can be encoded as a "move-wide" ')
            f.write("more efficiently as its registers are both <= %u\n" % (UINT4_MAX))
            return 4
        if self.regs[0] <= UINT8_MAX:
            f.write('warning: "move-wide/16" can be encoded as a ')
            f.write('"move-wide/from16" more efficiently as its first ')
            f.write("register is <= %u\n" % (UINT8_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode07(Opcode):
    ops = {0x07: "move-object"}
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode08(Opcode):
    ops = {0x08: "move-object/from16 "}
    num_code_units = 2
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst[1])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move-object/from16" can be encoded as a ')
            f.write('"move-object" more efficiently as its registers are ')
            f.write("both <= %u\n" % (UINT4_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode09(Opcode):
    ops = {0x09: "move-object/16"}
    num_code_units = 3
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst[1])
        self.regs.append(inst[2])

    def check_encoding(self, f=sys.stdout):
        if self.regs[0] <= UINT4_MAX and self.regs[1] <= UINT4_MAX:
            f.write('warning: "move-object/16" can be encoded as a ')
            f.write('"move-object" more efficiently as its registers ')
            f.write("are both <= %u\n" % (UINT4_MAX))
            return 4
        if self.regs[0] <= UINT8_MAX:
            f.write('warning: "move-object/16" can be encoded as a ')
            f.write('"move-object/from16" more efficiently as its first ')
            f.write("register is <= %u\n" % (UINT8_MAX))
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode0A_0D(Opcode):
    ops = {
        0x0A: "move-result",
        0x0B: "move-result-wide",
        0x0C: "move-result-object",
        0x0D: "move-exception",
    }
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode0E(Opcode):
    ops = {0x0E: "return-void"}
    num_code_units = 1
    max_regs = 0
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)

    def dump(self, f=sys.stdout, context=None):
        f.write("%s" % (self.get_name()))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode0F(Opcode):
    ops = {0x0F: "return"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode10(Opcode):
    ops = {0x10: "return-wide"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode11(Opcode):
    ops = {0x11: "return-object"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode12(Opcode):
    ops = {0x12: "const/4"}
    num_code_units = 1
    max_regs = 1
    extra_data = "n"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_A()
        self.imm = sign_extending(inst[0] >> 12, 4)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode13(Opcode):
    ops = {0x13: "const/16"}
    num_code_units = 2
    max_regs = 1
    extra_data = "s"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = sign_extending(inst[1], 16)

    def check_encoding(self, f=sys.stdout):
        if self.reg <= UINT4_MAX and INT4_MIN <= self.imm and self.imm <= INT4_MAX:
            f.write('warning: "const/16" can be encoded as a "const/4" more ')
            f.write("efficiently as its register is <= %u and " % (UINT4_MAX))
            f.write("(%i <= %i <= %i)\n" % (INT4_MIN, self.imm, INT4_MAX))
            return 2
        return 0

    def new_encoding(self, f=sys.stdout):
        if (
            self.reg <= UINT4_MAX
            and self.imm > INT4_MAX
            and self.imm <= (INT4_MAX + UINT4_MAX)
        ):
            f.write('"const/16" could be encoded as a new "const/u4" stores ')
            f.write("a 4 bit unsigned offset from +8 for a constant range ")
            f.write("of [8-24):\n")
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode14(Opcode):
    ops = {0x14: "const"}
    num_code_units = 3
    max_regs = 1
    extra_data = "i"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = inst.get_uint32(1)

    def check_encoding(self, f=sys.stdout):
        if self.reg <= UINT8_MAX and INT16_MIN <= self.imm and self.imm <= INT16_MAX:
            f.write('warning: "const" can be encoded as a "const/16" more ')
            f.write("efficiently as its register is < %u " % (UINT8_MAX))
            f.write("and (%i <= %i <= %i)\n" % (INT16_MIN, self.imm, INT16_MAX))
            return 2
        return 0

    def new_encoding(self, f=sys.stdout):
        if self.imm > INT16_MAX and self.imm <= (INT16_MAX + UINT16_MAX):
            f.write('"const" could be encoded as a new "const/u16" stores a ')
            f.write("16 bit unsigned offset from 32768 instead of a 16 bit ")
            f.write("signed value\n")
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode15(Opcode):
    ops = {0x15: "const/high16"}
    num_code_units = 2
    max_regs = 1
    extra_data = "h"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = inst[1] << 16

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode16(Opcode):
    ops = {0x16: "const-wide/16"}
    num_code_units = 2
    max_regs = 1
    extra_data = "s"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = sign_extending(inst[1], 16)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode17(Opcode):
    ops = {0x17: "const-wide/32"}
    num_code_units = 3
    max_regs = 1
    extra_data = "i"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = inst.get_sint32(1)

    def check_encoding(self, f=sys.stdout):
        if INT16_MIN <= self.imm and self.imm <= INT16_MAX:
            f.write('warning: "const-wide/32" can be encoded as a ')
            f.write(
                '"const-wide/16" more efficiently as (%i <= %i <= %i)\n'
                % (INT16_MIN, self.imm, INT16_MAX)
            )
            return 2
        return 0

    def new_encoding(self, f=sys.stdout):
        if self.imm > INT16_MAX and self.imm <= (INT16_MAX + UINT16_MAX):
            f.write('"const-wide/32" could be encoded as a new ')
            f.write('"const-wide/u16" stores a 16 bit unsigned offset from ')
            f.write("32768 instead of a 16 bit signed value\n")
            return 2
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode18(Opcode):
    ops = {0x18: "const-wide/64"}
    num_code_units = 5
    max_regs = 1
    extra_data = "l"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = inst.get_uint64(1)

    def check_encoding(self, f=sys.stdout):
        if INT16_MIN <= self.imm and self.imm <= INT16_MAX:
            f.write('warning: "const-wide/64" can be encoded as a ')
            f.write(
                '"const-wide/16" more efficiently as (%i <= %i <= %i)\n'
                % (INT16_MIN, self.imm, INT16_MAX)
            )
            return 6
        if INT32_MIN <= self.imm and self.imm <= INT32_MAX:
            f.write('warning: "const-wide/64" can be encoded as a ')
            f.write(
                '"const-wide/32" more efficiently as (%i <= %i <= %i)\n'
                % (INT32_MIN, self.imm, INT32_MAX)
            )
            return 4
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode19(Opcode):
    ops = {0x19: "const-wide/high16"}
    num_code_units = 2
    max_regs = 1
    extra_data = "h"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.imm = sign_extending(inst[1], 16) << 48

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, #int %i // #%#x" % (self.get_name(), self.reg, self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class Opcode1A(Opcode):
    ops = {0x1A: "const-string"}
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.string_idx = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            f.write('"%s" // ' % context.get_string(self.string_idx))
        f.write("string@%4.4x" % self.string_idx)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode1B(Opcode):
    ops = {0x1B: "const-string/jumbo"}
    num_code_units = 3
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.string_idx = inst.get_uint32(1)

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            f.write('"%s" // ' % context.get_string(self.string_idx))
        f.write("string@%8.8x" % self.string_idx)

    def check_encoding(self, f=sys.stdout):
        if self.signed_offset <= UINT16_MAX:
            f.write('warning: "const-string/jumbo" can be encoded as a ')
            f.write('"const-string" more efficiently as its offset is ')
            f.write("<= UINT16_MAX\n")
            return 2
        return 0

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode1C(Opcode):
    ops = {0x1C: "const-class"}
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.type = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode1D(Opcode):
    ops = {0x1D: "monitor-enter"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode1E(Opcode):
    ops = {0x1E: "monitor-exit"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode1F(Opcode):
    ops = {0x1F: "check-cast"}
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.type = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode20(Opcode):
    ops = {0x20: "instance-of"}
    num_code_units = 2
    max_regs = 2
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())
        self.type = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u, " % (self.get_name(), self.regs[0], self.regs[1]))
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode21(Opcode):
    ops = {0x21: "array-length"}
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode22(Opcode):
    ops = {0x22: "new-instance"}
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.type = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode23(Opcode):
    ops = {0x23: "new-array"}
    num_code_units = 2
    max_regs = 2
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())
        self.type = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u " % (self.get_name(), self.regs[0], self.regs[1]))
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode24(Opcode):
    ops = {0x24: "filled-new-array"}
    num_code_units = 3
    max_regs = 5
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        arg_count = inst[0] >> 12
        self.type = inst[1]
        self.regs = []
        regs = inst[2] | ((inst[0] << 8) & 0xF0000)
        for _ in range(arg_count):
            self.regs.append(regs & 0xF)
            regs >>= 4

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s {%s} "
            % (self.get_name(), ", ".join(["v%u" % reg for reg in self.regs]))
        )
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode25(Opcode):
    ops = {0x25: "filled-new-array/range "}
    num_code_units = 3
    max_regs = "r"
    extra_data = "c"
    format = "3rc"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        arg_count = inst.get_AA()
        self.type = inst[1]
        first_reg = inst[2]
        self.regs = []
        for i in range(arg_count):
            self.regs.append(first_reg + i)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s {%s} "
            % (self.get_name(), ", ".join(["v%u" % reg for reg in self.regs]))
        )
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode26(Opcode):
    ops = {0x26: "fill-array-data"}
    num_code_units = 3
    max_regs = 1
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.signed_offset = inst.get_sint32(1)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, %8.8x // %s"
            % (
                self.get_name(),
                self.reg,
                self.inst.code_unit_idx + self.signed_offset,
                get_signed_hex_offset_as_str(self.signed_offset, 8),
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode27(Opcode):
    ops = {0x27: "throw"}
    num_code_units = 1
    max_regs = 1
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u" % (self.get_name(), self.reg))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode28(Opcode):
    ops = {0x28: "goto"}
    num_code_units = 1
    max_regs = 0
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.signed_offset = inst.get_signed_AA()

    def check_encoding(self, f=sys.stdout):
        if self.signed_offset == 0:
            f.write('error: "goto" has a zero offset  (invalid encoding)\n')
        return 0

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s %4.4x // %+i"
            % (
                self.get_name(),
                self.inst.code_unit_idx + self.signed_offset,
                self.signed_offset,
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode29(Opcode):
    ops = {0x29: "goto/16"}
    num_code_units = 2
    max_regs = 0
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.signed_offset = sign_extending(inst[1], 16)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s %4.4x // %+i"
            % (
                self.get_name(),
                self.inst.code_unit_idx + self.signed_offset,
                self.signed_offset,
            )
        )

    def check_encoding(self, f=sys.stdout):
        if self.signed_offset == 0:
            f.write('error: "goto/16" has a zero offset (invalid encoding)\n')
        elif INT8_MIN <= self.signed_offset and self.signed_offset <= INT8_MAX:
            f.write('warning: "goto/16" can be encoded as a "goto" more ')
            f.write("efficiently since (INT8_MIN <= offset <= INT8_MAX)\n")
            return 2
        return 0

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode2A(Opcode):
    ops = {0x2A: "goto/32"}
    num_code_units = 3
    max_regs = 0
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.signed_offset = inst.get_sint32(1)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s %4.4x // %+i"
            % (
                self.get_name(),
                self.inst.code_unit_idx + self.signed_offset,
                self.signed_offset,
            )
        )

    def check_encoding(self, f=sys.stdout):
        if self.signed_offset == 0:
            return 0
        if INT8_MIN <= self.signed_offset and self.signed_offset <= INT8_MAX:
            f.write('warning: "goto/32" can be encoded as a "goto" more ')
            f.write("efficiently since (INT8_MIN <= offset <= INT8_MAX)\n")
            return 2
        if INT16_MIN <= self.signed_offset and self.signed_offset <= INT16_MAX:
            f.write('warning: "goto/32" can be encoded as a "goto/16" more ')
            f.write("efficiently since (INT16_MIN <= offset <= INT16_MAX)\n")
            return 4
        return 0

    def new_encoding(self, f=sys.stdout):
        if INT16_MIN <= self.signed_offset and self.signed_offset <= INT16_MAX:
            return 0
        if INT24_MIN <= self.signed_offset and self.signed_offset <= INT24_MAX:
            f.write('"goto/32" could be encoded as a new "goto/16" where ')
            f.write("that opcode uses the extra 8 bits in the first code ")
            f.write("unit to provide a 24 bit branch range\n")
            return 2
        return 0

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode2B(Opcode):
    ops = {0x2B: "packed-switch"}
    num_code_units = 3
    max_regs = 1
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.branch = inst.get_sint32(1)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, %8.8x // +%8.8x"
            % (
                self.get_name(),
                self.reg,
                self.inst.get_code_unit_index() + self.branch,
                self.branch,
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode2C(Opcode):
    ops = {0x2C: "sparse-switch"}
    num_code_units = 3
    max_regs = 1
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.branch = inst.get_sint32(1)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, %8.8x // +%8.8x"
            % (
                self.get_name(),
                self.reg,
                self.inst.get_code_unit_index() + self.branch,
                self.branch,
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode2D_31(Opcode):
    ops = {
        0x2D: "cmpl-float (lt bias)",
        0x2E: "cmpg-float (gt bias)",
        0x2F: "cmpl-double (lt bias)",
        0x30: "cmpg-double (gt bias)",
        0x31: "cmp-long",
    }
    num_code_units = 2
    max_regs = 3
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst.get_uint8_lo(1))
        self.regs.append(inst.get_uint8_hi(1))

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, v%u"
            % (self.get_name(), self.regs[0], self.regs[1], self.regs[2])
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode32_37(Opcode):
    ops = {
        0x32: "if-eq",
        0x33: "if-ne",
        0x34: "if-lt",
        0x35: "if-ge",
        0x36: "if-gt",
        0x37: "if-le",
    }
    num_code_units = 2
    max_regs = 2
    extra_data = "t"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())
        self.signed_offset = sign_extending(inst[1], 16)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, %4.4x // %i"
            % (
                self.get_name(),
                self.regs[0],
                self.regs[1],
                self.inst.code_unit_idx + self.signed_offset,
                self.signed_offset,
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode38_3D(Opcode):
    ops = {
        0x38: "if-eqz",
        0x39: "if-nez",
        0x3A: "if-ltz",
        0x3B: "if-gez",
        0x3C: "if-gtz",
        0x3D: "if-lez",
    }
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.signed_offset = sign_extending(inst[1], 16)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, %4.4x // %s"
            % (
                self.get_name(),
                self.reg,
                self.signed_offset + self.inst.code_unit_idx,
                get_signed_hex_offset_as_str(self.signed_offset, 4),
            )
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode44_51(Opcode):
    ops = {
        0x44: "aget",
        0x45: "aget-wide",
        0x46: "aget-object",
        0x47: "aget-boolean",
        0x48: "aget-byte",
        0x49: "aget-char",
        0x4A: "aget-short",
        0x4B: "aput",
        0x4C: "aput-wide",
        0x4D: "aput-object",
        0x4E: "aput-boolean",
        0x4F: "aput-byte",
        0x50: "aput-char",
        0x51: "aput-short",
    }
    num_code_units = 2
    max_regs = 3
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst.get_uint8_lo(1))
        self.regs.append(inst.get_uint8_hi(1))

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, v%u"
            % (self.get_name(), self.regs[0], self.regs[1], self.regs[2])
        )

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode52_5f(Opcode):
    ops = {
        0x52: "iget",
        0x53: "iget-wide",
        0x54: "iget-object",
        0x55: "iget-boolean",
        0x56: "iget-byte",
        0x57: "iget-char",
        0x58: "iget-short",
        0x59: "iput",
        0x5A: "iput-wide",
        0x5B: "iput-object",
        0x5C: "iput-boolean",
        0x5D: "iput-byte",
        0x5E: "iput-char",
        0x5F: "iput-short",
    }
    num_code_units = 2
    max_regs = 2
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())
        self.field_idx = inst[1]

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u, " % (self.get_name(), self.regs[0], self.regs[1]))
        if context is not None:
            field_item = context.get_field_ids()[self.field_idx]
            f.write(
                "%s.%s:%s // "
                % (
                    context.get_typename(field_item.class_idx),
                    context.get_string(field_item.name_idx),
                    context.get_typename(field_item.type_idx),
                )
            )
        f.write("field@%4.4x" % self.field_idx)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode60_6d(Opcode):
    ops = {
        0x60: "sget",
        0x61: "sget-wide",
        0x62: "sget-object",
        0x63: "sget-boolean",
        0x64: "sget-byte",
        0x65: "sget-char",
        0x66: "sget-short",
        0x67: "sput",
        0x68: "sput-wide",
        0x69: "sput-object",
        0x6A: "sput-boolean",
        0x6B: "sput-byte",
        0x6C: "sput-char",
        0x6D: "sput-short",
    }
    num_code_units = 2
    max_regs = 1
    extra_data = "c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.reg = inst.get_AA()
        self.field_idx = inst.get_uint16(1)

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, " % (self.get_name(), self.reg))
        if context is not None:
            field_item = context.get_field_ids()[self.field_idx]
            f.write(
                "%s.%s:%s // "
                % (
                    context.get_typename(field_item.class_idx),
                    context.get_string(field_item.name_idx),
                    context.get_typename(field_item.type_idx),
                )
            )
        f.write("field@%4.4x" % self.field_idx)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


can_use_new_encoding = 0
cant_use_new_encoding = 0


class Opcode6E_72(Opcode):
    ops = {
        0x6E: "invoke-virtual",
        0x6F: "invoke-super",
        0x70: "invoke-direct",
        0x71: "invoke-static",
        0x72: "invoke-interface",
    }
    num_code_units = 3
    max_regs = 5
    extra_data = "c"
    format = "35c"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        arg_count = inst[0] >> 12
        self.method_idx = inst[1]
        self.regs = []
        regs = inst[2] | ((inst[0] << 8) & 0xF0000)
        for _ in range(arg_count):
            self.regs.append(regs & 0xF)
            regs >>= 4

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s {%s} "
            % (self.get_name(), ", ".join(["v%u" % reg for reg in self.regs]))
        )
        if context is not None:
            method_item = context.get_method_ids()[self.method_idx]
            f.write(
                "%s.%s:%s // "
                % (
                    context.get_typename(method_item.class_idx),
                    context.get_string(method_item.name_idx),
                    context.get_proto_string(method_item.proto_idx),
                )
            )
        f.write("method@%4.4x" % self.method_idx)

    def new_encoding(self, f=sys.stdout):
        if (
            self.regs_are_sequential()
            and (len(self.regs) == 0 or self.regs[0] <= UINT4_MAX)
            and len(self.regs) <= UINT4_MAX
        ):
            global can_use_new_encoding
            can_use_new_encoding += 1
            name = self.get_name()
            f.write('"%s" can be encoded as "%s/min-range" ' % (name, name))
            f.write("where the first register is contained in the first ")
            f.write("opcode\n")
            return 2
        global cant_use_new_encoding
        cant_use_new_encoding += 1
        return 0

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode74_78(Opcode):
    ops = {
        0x74: "invoke-virtual/range",
        0x75: "invoke-super/range",
        0x76: "invoke-direct/range",
        0x77: "invoke-static/range",
        0x78: "invoke-interface/range",
    }
    num_code_units = 3
    max_regs = "r"
    extra_data = "c"
    format = "3rc"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        arg_count = inst.get_AA()
        self.method_idx = inst[1]
        first_reg = inst[2]
        self.regs = []
        for i in range(arg_count):
            self.regs.append(first_reg + i)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s {%s} "
            % (self.get_name(), ", ".join(["v%u" % reg for reg in self.regs]))
        )
        if context is not None:
            method_item = context.get_method_ids()[self.method_idx]
            f.write(
                "%s.%s:%s // "
                % (
                    context.get_typename(method_item.class_idx),
                    context.get_string(method_item.name_idx),
                    context.get_proto_string(method_item.proto_idx),
                )
            )
        f.write("method@%4.4x" % self.method_idx)

    def new_encoding(self, f=sys.stdout):
        if (
            self.regs_are_sequential()
            and (len(self.regs) == 0 or self.regs[0] <= UINT4_MAX)
            and len(self.regs) <= UINT4_MAX
        ):
            name = self.get_name()
            f.write('"%s" can be encoded as a "%s/min-range" ' % (name, name))
            f.write("where the first register is contained in the first ")
            f.write("opcode\n")
            return 2
        return 0

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode7B_8F(Opcode):
    ops = {
        0x7B: "neg-int",
        0x7C: "not-int",
        0x7D: "neg-long",
        0x7E: "not-long",
        0x7F: "neg-float",
        0x80: "neg-double",
        0x81: "int-to-long",
        0x82: "int-to-float",
        0x83: "int-to-double",
        0x84: "long-to-int",
        0x85: "long-to-float",
        0x86: "long-to-double",
        0x87: "float-to-int",
        0x88: "float-to-long",
        0x89: "float-to-double",
        0x8A: "double-to-int",
        0x8B: "double-to-long",
        0x8C: "double-to-float",
        0x8D: "int-to-byte",
        0x8E: "int-to-char",
        0x8F: "int-to-short",
    }
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class Opcode90_AF(Opcode):
    ops = {
        0x90: "add-int",
        0x91: "sub-int",
        0x92: "mul-int",
        0x93: "div-int",
        0x94: "rem-int",
        0x95: "and-int",
        0x96: "or-int",
        0x97: "xor-int",
        0x98: "shl-int",
        0x99: "shr-int",
        0x9A: "ushr-int",
        0x9B: "add-long",
        0x9C: "sub-long",
        0x9D: "mul-long",
        0x9E: "div-long",
        0x9F: "rem-long",
        0xA0: "and-long",
        0xA1: "or-long",
        0xA2: "xor-long",
        0xA3: "shl-long",
        0xA4: "shr-long",
        0xA5: "ushr-long",
        0xA6: "add-float",
        0xA7: "sub-float",
        0xA8: "mul-float",
        0xA9: "div-float",
        0xAA: "rem-float",
        0xAB: "add-double",
        0xAC: "sub-double",
        0xAD: "mul-double",
        0xAE: "div-double",
        0xAF: "rem-double",
    }
    num_code_units = 2
    max_regs = 3
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst.get_uint8_lo(1))
        self.regs.append(inst.get_uint8_hi(1))

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, v%u"
            % (self.get_name(), self.regs[0], self.regs[1], self.regs[2])
        )

    def opIsCommutative(self):
        """Return True if the operation is commutative"""
        op = self.get_op()
        return (
            op == 0x90  # add-int
            or op == 0x92  # mul-int
            or op == 0x95  # and-int
            or op == 0x96  # or-int
            or op == 0x97  # xor-int
            or op == 0x9B  # add-long
            or op == 0x9D  # mul-long
            or op == 0xA0  # and-long
            or op == 0xA1  # or-long
            or op == 0xA2  # xor-long
            or op == 0xA6  # add-float
            or op == 0xA8  # mul-float
            or op == 0xAB  # add-double
            or op == 0xAD  # mul-double
        )

    def check_encoding(self, f=sys.stdout):
        vAA = self.regs[0]
        vBB = self.regs[1]
        vCC = self.regs[2]
        if vAA == vBB and vAA <= UINT4_MAX and vCC <= UINT4_MAX:
            name = self.get_name()
            f.write('warning: "%s" can be encoded more efficiently ' % (name))
            f.write('as "%s/2addr v%u, v%u"\n' % (name, vAA, vCC))
            return 2
        if (
            vAA == vCC
            and vAA <= UINT4_MAX
            and vBB <= UINT4_MAX
            and self.opIsCommutative()
        ):
            name = self.get_name()
            f.write('warning: "%s" is commutative and can be ' % (name))
            f.write(
                'encoded more efficiently as "%s/2addr v%u, v%u"\n' % (name, vAA, vBB)
            )
            return 2
        return 0  # Return zero to indicate we can't save any bytes

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class OpcodeB0_CF(Opcode):
    ops = {
        0xB0: "add-int/2addr",
        0xB1: "sub-int/2addr",
        0xB2: "mul-int/2addr",
        0xB3: "div-int/2addr",
        0xB4: "rem-int/2addr",
        0xB5: "and-int/2addr",
        0xB6: "or-int/2addr",
        0xB7: "xor-int/2addr",
        0xB8: "shl-int/2addr",
        0xB9: "shr-int/2addr",
        0xBA: "ushr-int/2addr",
        0xBB: "add-long/2addr",
        0xBC: "sub-long/2addr",
        0xBD: "mul-long/2addr",
        0xBE: "div-long/2addr",
        0xBF: "rem-long/2addr",
        0xC0: "and-long/2addr",
        0xC1: "or-long/2addr",
        0xC2: "xor-long/2addr",
        0xC3: "shl-long/2addr",
        0xC4: "shr-long/2addr",
        0xC5: "ushr-long/2addr",
        0xC6: "add-float/2addr",
        0xC7: "sub-float/2addr",
        0xC8: "mul-float/2addr",
        0xC9: "div-float/2addr",
        0xCA: "rem-float/2addr",
        0xCB: "add-double/2addr",
        0xCC: "sub-double/2addr",
        0xCD: "mul-double/2addr",
        0xCE: "div-double/2addr",
        0xCF: "rem-double/2addr ",
    }
    num_code_units = 1
    max_regs = 2
    extra_data = "x"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())

    def dump(self, f=sys.stdout, context=None):
        f.write("%s v%u, v%u" % (self.get_name(), self.regs[0], self.regs[1]))

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class OpcodeD0_D7(Opcode):
    ops = {
        0xD0: "add-int/lit16",
        0xD1: "rsub-int/lit16",
        0xD2: "mul-int/lit16",
        0xD3: "div-int/lit16",
        0xD4: "rem-int/lit16",
        0xD5: "and-int/lit16",
        0xD6: "or-int/lit16",
        0xD7: "xor-int/lit16",
    }
    num_code_units = 2
    max_regs = 2
    extra_data = "s"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_A())
        self.regs.append(inst.get_B())
        self.imm = sign_extending(inst[1], 16)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, #int %i // #%#x"
            % (self.get_name(), self.regs[0], self.regs[1], self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class OpcodeD8_E2(Opcode):
    ops = {
        0xD8: "add-int/lit8",
        0xD9: "rsub-int/lit8",
        0xDA: "mul-int/lit8",
        0xDB: "div-int/lit8",
        0xDC: "rem-int/lit8",
        0xDD: "and-int/lit8",
        0xDE: "or-int/lit8",
        0xDF: "xor-int/lit8",
        0xE0: "shl-int/lit8",
        0xE1: "shr-int/lit8",
        0xE2: "ushr-int/lit8",
    }
    num_code_units = 2
    max_regs = 2
    extra_data = "b"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        self.regs = []
        self.regs.append(inst.get_AA())
        self.regs.append(inst.get_uint8_lo(1))
        self.imm = sign_extending(inst.get_uint8_hi(1), 8)

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s v%u, v%u, #int %i // #%#x"
            % (self.get_name(), self.regs[0], self.regs[1], self.imm, self.imm)
        )

    def emulate(self, emulator):
        emulator.write_register(self.reg, self.imm)


class OpcodeFA(Opcode):
    ops = {0xFA: "invoke-polymorphic"}
    num_code_units = 4
    max_regs = 5
    extra_data = "cc"

    def __init__(self, inst, code_units):
        Opcode.__init__(self, inst)
        raise ValueError("debug this when we find one of these")
        arg_count = inst[0] >> 12
        self.method_ref_idx = inst[1]
        self.method_hdl_ref = inst[2]
        self.regs = []
        regs = inst[3] | ((inst[0] << 8) & 0xF0000)
        self.proto = inst[4]
        for _ in range(arg_count):
            self.regs.append(regs & 0xF)
            regs >>= 4

    def dump(self, f=sys.stdout, context=None):
        f.write(
            "%s {%s} "
            % (self.get_name(), ", ".join(["v%u" % reg for reg in self.regs]))
        )
        if context is not None:
            f.write("%s // " % context.get_typename(self.type))
        f.write("type@%4.4x" % self.type)

    def emulate(self, emulator):
        raise ValueError("emulate not supported")


class CodeUnits(Opcode):
    def __init__(self, code_units):
        self.code_units = code_units
        self.idx = 0

    def index_is_valid(self):
        return self.idx < len(self.code_units)

    def get_index(self):
        return self.idx

    def peek_code_unit(self, idx):
        return self.code_units[idx]

    def get_int(self):
        return sign_extending(self.get_uint(), 32)

    def get_uint(self):
        return self.get_code_unit() | (self.get_code_unit() << 16)

    def get_code_unit(self):
        idx = self.idx
        self.idx += 1
        return self.code_units[idx]


def swap16(u):
    return ((u >> 8) & 0x00FF) | ((u << 8) & 0xFF00)


class DexInstruction(object):
    opcode_defs = []

    @classmethod
    def initialize(cls):
        opcode_classes = [
            Opcode00,
            Opcode01,
            Opcode02,
            Opcode03,
            Opcode04,
            Opcode05,
            Opcode06,
            Opcode07,
            Opcode08,
            Opcode09,
            Opcode0A_0D,
            Opcode0E,
            Opcode0F,
            Opcode10,
            Opcode11,
            Opcode12,
            Opcode13,
            Opcode14,
            Opcode15,
            Opcode16,
            Opcode17,
            Opcode18,
            Opcode19,
            Opcode1A,
            Opcode1B,
            Opcode1C,
            Opcode1D,
            Opcode1E,
            Opcode1F,
            Opcode20,
            Opcode21,
            Opcode22,
            Opcode23,
            Opcode24,
            Opcode25,
            Opcode26,
            Opcode27,
            Opcode28,
            Opcode29,
            Opcode2A,
            Opcode2B,
            Opcode2C,
            Opcode2D_31,
            Opcode32_37,
            Opcode38_3D,
            Opcode44_51,
            Opcode52_5f,
            Opcode60_6d,
            Opcode6E_72,
            Opcode74_78,
            Opcode7B_8F,
            Opcode90_AF,
            OpcodeB0_CF,
            OpcodeD0_D7,
            OpcodeD8_E2,
            OpcodeFA,
        ]
        for _ in range(256):
            cls.opcode_defs.append(None)
        for opcode_class in opcode_classes:
            for op in opcode_class.ops:
                if cls.opcode_defs[op] is None:
                    cls.opcode_defs[op] = opcode_class
                else:
                    raise ValueError(
                        "registering the same opcode twice: "
                        "%#2.2x in %s" % (op, str(opcode_class))
                    )

    def dump(self, f=sys.stdout, suffix="\n", context=None):
        f.write("%4.4x:" % (self.code_unit_idx))
        for code_unit in self.code_units:
            f.write(" %4.4x" % (swap16(code_unit)))
        num_code_units = len(self.code_units)
        if num_code_units < 5:
            pad = 5 - num_code_units
            for _ in range(pad):
                f.write("     ")
        f.write(" ")
        self.instruction.dump(f=f, context=context)
        if suffix:
            f.write(suffix)

    def __init__(self):
        self.code_unit_idx = -1
        self.code_units = None

    def check_encoding(self, f=sys.stdout):
        bytes_saved = self.instruction.check_encoding(f)
        if bytes_saved:
            self.dump(f)
        return bytes_saved

    def new_encoding(self, f=sys.stdout):
        bytes_saved = self.instruction.new_encoding(f)
        if bytes_saved:
            self.dump(f)
        return bytes_saved

    def get_code_unit_index(self):
        return self.code_unit_idx

    def decode(self, code_units):
        self.code_unit_idx = code_units.get_index()
        self.code_units = []
        self.code_units.append(code_units.get_code_unit())
        op = self.get_op()
        opcode_class = self.opcode_defs[op]
        if opcode_class is None:
            raise ValueError("unsupported opcode %#4.4x" % (swap16(self[0])))
        for _ in range(1, opcode_class.num_code_units):
            self.code_units.append(code_units.get_code_unit())
        self.instruction = opcode_class(self, code_units)

    def get_name(self):
        return self.instruction.get_name()

    def get_num_code_units(self):
        return self.instruction.get_num_code_units()

    def get_op(self):
        """Return the 1 byte op field that tells us what instruction this is"""
        return self.code_units[0] & 0xFF

    def get_A(self):
        """Get the 4 bit value of A"""
        return (self.code_units[0] >> 8) & 0xF

    def get_B(self):
        """Get the 4 bit value of B"""
        return (self.code_units[0] >> 12) & 0xF

    def get_AA(self):
        """Get the 8 bit value of AA from the byte next to the Op"""
        return self.get_uint8_hi(0)

    def get_signed_AA(self):
        return sign_extending(self.get_AA(), 8)

    def get_uint8_lo(self, idx):
        return self.code_units[idx] & 0xFF

    def get_sint8_lo(self, idx):
        return sign_extending(self.get_uint8_lo(), 8)

    def get_uint8_hi(self, idx):
        return (self.code_units[idx] >> 8) & 0xFF

    def get_sint8_hi(self, idx):
        return sign_extending(self.get_uint8_hi(), 8)

    def get_uint16(self, idx):
        return self.code_units[idx]

    def get_sint16(self, idx):
        return sign_extending(self.get_uint16(), 16)

    def get_uint32(self, idx):
        return self.code_units[idx + 1] << 16 | self.code_units[idx]

    def get_sint32(self, idx):
        return sign_extending(self.get_uint32(idx), 32)

    def get_uint64(self, idx):
        return (
            self.code_units[idx + 3] << 48
            | self.code_units[idx + 2] << 32
            | self.code_units[idx + 1] << 16
            | self.code_units[idx]
        )

    def get_sint64(self, idx):
        return sign_extending(self.get_uint64(idx), 64)

    def __len__(self):
        """Overload the length operator to give out the number of code units"""
        return len(self.code_units)

    def __getitem__(self, key):
        """Overload the [] operator to give out code units"""
        return self.code_units[key]

    def emulate(self, emulator):
        self.instruction.emulate(emulator)


DexInstruction.initialize()


def get_percentage(part, total):
    return (float(part) / float(total)) * 100.0


def print_code_stats(size, total_size, file_size):
    code_savings = get_percentage(size, total_size)
    file_savings = get_percentage(size, file_size)
    print(
        "error: %u of %u code bytes (%u file bytes) " % (size, total_size, file_size),
        end="",
    )
    print("could be saved by encoding opcodes more efficiently ", end="")
    print(
        "(%2.2f%% code savings, %2.2f%% file savings).\n" % (code_savings, file_savings)
    )


def print_debug_stats(size, file_size):
    file_savings = get_percentage(size, file_size)
    print("error: %u debug info bytes of %u file " % (size, file_size), end="")
    print("bytes could be saved by encoding debug info more ", end="")
    print("efficiently (%2.2f%% file savings).\n" % (file_savings))


def print_encoding_stats(size, total_size, file_size):
    code_savings = get_percentage(size, total_size)
    file_savings = get_percentage(size, file_size)
    print("%u of %u code bytes could be saved " % (size, total_size), end="")
    print("could be saved by encoding opcodes more efficiently ", end="")
    print(
        "(%2.2f%% code savings, %2.2f%% file savings).\n" % (code_savings, file_savings)
    )


class DexEmulator(object):
    def __init__(self):
        self.registers = {}
        self.pc = 0

    def read_register(self, reg):
        if reg in self.registers:
            return self.registers[reg]
        raise ValueError("reading register with no value")

    def write_register(self, reg, value):
        self.registers[reg] = value

    def emulate(self, uint16_array):
        pass


def main():
    usage = "Usage: dex.py [options] [dex file(s)]"
    parser = optparse.OptionParser(
        usage=usage, description="A script that parses DEX files."
    )
    parser.add_option(
        "-v",
        "--verbose",
        action="store_true",
        dest="verbose",
        help="display verbose debug info",
        default=False,
    )
    parser.add_option(
        "-C",
        "--color",
        action="store_true",
        dest="color",
        help="Enable colorized output",
        default=False,
    )
    parser.add_option(
        "-a",
        "--all",
        action="store_true",
        dest="dump_all",
        help="Dump all DEX sections.",
        default=False,
    )
    parser.add_option(
        "-H",
        "--header",
        action="store_true",
        dest="dump_header",
        help="Dump the DEX file header.",
        default=False,
    )
    parser.add_option(
        "--map-list",
        action="store_true",
        dest="dump_map_list",
        help="Dump the DEX map list info.",
        default=False,
    )
    parser.add_option(
        "-s",
        "--strings",
        action="store_true",
        dest="dump_strings",
        help="Dump the DEX strings.",
        default=False,
    )
    parser.add_option(
        "-t",
        "--types",
        action="store_true",
        dest="dump_types",
        help="Dump the DEX types.",
        default=False,
    )
    parser.add_option(
        "-p",
        "--protos",
        action="store_true",
        dest="dump_protos",
        help="Dump the DEX protos.",
        default=False,
    )
    parser.add_option(
        "-f",
        "--fields",
        action="store_true",
        dest="dump_fields",
        help="Dump the DEX fields.",
        default=False,
    )
    parser.add_option(
        "-m",
        "--methods",
        action="store_true",
        dest="dump_methods",
        help="Dump the DEX methods.",
        default=False,
    )
    parser.add_option(
        "--method-handles",
        action="store_true",
        dest="dump_method_handles",
        help="Dump the DEX method handles.",
        default=False,
    )
    parser.add_option(
        "--class-list",
        action="store_true",
        dest="dump_class_list",
        help="Dump the list of DEX classes.",
        default=False,
    )
    parser.add_option(
        "--classes",
        action="store_true",
        dest="dump_classes",
        help="Dump the DEX classes.",
        default=False,
    )
    parser.add_option(
        "--class",
        dest="class_filter",
        help="Find a class by name. " + "Accepts `Lpath/to/Class;` or `path.to.Class`",
        default=None,
    )
    parser.add_option(
        "--method",
        dest="method_filter",
        help="Find a method by name. Must be used with --class",
        default=None,
    )
    parser.add_option(
        "--call-sites",
        action="store_true",
        dest="dump_call_sites",
        help="Dump the DEX call sites.",
        default=False,
    )
    parser.add_option(
        "--code",
        action="store_true",
        dest="dump_code",
        help="Dump the DEX code in all class methods.",
        default=False,
    )
    parser.add_option(
        "--code-items",
        action="store_true",
        dest="dump_code_items",
        help="Dump the DEX code items.",
        default=False,
    )
    parser.add_option(
        "--code-duplication",
        action="store_true",
        dest="code_duplication",
        help=("Dump any methods in the DEX file that have the " "same instructions."),
        default=False,
    )
    parser.add_option(
        "--debug-info",
        action="store_true",
        dest="dump_debug_info",
        help="Dump the DEX debug info for each method.",
        default=False,
    )
    parser.add_option(
        "--debug-info-items",
        action="store_true",
        dest="dump_debug_info_items",
        help="Dump the DEX debug info items pointed to in its" + " map_list",
        default=False,
    )
    parser.add_option(
        "--stats",
        action="store_true",
        dest="dump_stats",
        help="Dump the DEX opcode statistics.",
        default=False,
    )
    parser.add_option(
        "--check-encoding",
        action="store_true",
        dest="check_encoding",
        help="Verify opcodes are efficiently encoded.",
        default=False,
    )
    parser.add_option(
        "--new-encoding",
        action="store_true",
        dest="new_encoding",
        help="Report byte savings from potential new encodings.",
        default=False,
    )
    parser.add_option(
        "--proguard",
        dest="proguard",
        help="Specify a progard file to use for demangling.",
        default=None,
    )
    parser.add_option(
        "--skip-abstract",
        action="store_true",
        dest="skip_abstract",
        help="Don't print information coming from abstract"
        " classes when passing --code, --debug or --all.",
        default=False,
    )
    parser.add_option(
        "--counts",
        action="store_true",
        dest="dump_counts",
        help="Dump the DEX opcode counts",
        default=False,
    )
    parser.add_option(
        "--use-bytecode-format",
        action="store_true",
        dest="use_bytecode_format",
        help="When passed, switch from java to bytecode format.",
    )
    parser.add_option(
        "--public-only",
        action="store_true",
        dest="public_only",
        help="Only dump classes / methods / fields that are public",
        default=False,
    )
    parser.add_option(
        "--dump-structure",
        action="store_true",
        dest="dump_structure",
        help="Dumps just the names of all classes / methods / fields",
        default=False,
    )
    (options, files) = parser.parse_args()

    total_code_bytes_inefficiently_encoded = 0
    total_debug_info_bytes_inefficiently_encoded = 0
    total_new_code_bytes_inefficiently_encoded = 0
    total_opcode_byte_size = 0
    total_file_size = 0
    op_name_to_size = {}
    op_name_to_count = {}
    string_counts = {}
    i = 0

    if len(files) == 0:
        print("No input files. {}".format(usage))
        return

    def generate_dex_objects(files):
        for path in files:
            base = os.path.basename(path)
            ext = os.path.splitext(path)[1]

            def handle_zip(zip_file, path, name):
                # Naive implementation uses ZipFile entries which are file-like:
                #   info = zip_file.getinfo(name)
                #   return (path, info.file_size, file.open(info))
                # Problem is that performance is abysmal. So we unpack into
                # memory.
                info = zip_file.getinfo(name)
                data = zip_file.read(info)
                return (path, info.file_size, io.BytesIO(data))

            # Special handling for direct zip access.
            if "!" in base and ext == ".dex":
                zip_path = os.path.join(os.path.dirname(path), base[0 : base.find("!")])
                name = base[base.find("!") + 1 :]
                file = zipfile.ZipFile(zip_path, "r")
                names = set(file.namelist())
                if name not in names:
                    print("%s does not contain %s" % (zip_path, name))
                    break
                yield handle_zip(file, path, name)
                continue

            if ext == ".dex":
                # Plain dex file, open as file.
                yield (path, os.path.getsize(path), open(path, "rb"))
                continue

            if ext == ".apk" or ext == ".jar" or ext == ".zip":
                file = zipfile.ZipFile(path, "r")
                names = set(file.namelist())
                if "classes.dex" not in names:
                    print("%s does not contain classes.dex" % path)
                    break
                yield handle_zip(file, path + "!classes.dex", "classes.dex")
                for i in range(2, 100000):
                    name = "classes%d.dex" % i
                    if name not in names:
                        break
                    yield handle_zip(file, path + "!" + name, name)
                continue

            print("error: dex.py does not know how to handle %s" % path)
            break

    for path, file_size, file_like in generate_dex_objects(files):
        print("Dex file: %s" % (path))
        total_file_size += file_size
        dex = File(path, file_like, options.proguard, options.use_bytecode_format)
        if options.class_filter:
            dex_class = dex.find_class(options.class_filter)
            if dex_class:
                if options.method_filter is None:
                    dex_class.dump(options)
                for method in dex_class.get_methods():
                    method_name = method.get_name()
                    if options.method_filter:
                        if options.method_filter != method_name:
                            continue
                    method.dump(options)
            else:
                print(
                    'error: class definition not found for "%s"'
                    % (options.class_filter)
                )
        if options.dump_classes:
            for dex_class in dex.get_classes():
                dex_class.dump(options)
                for method in dex_class.get_methods():
                    method.dump(options)

        if options.dump_header or options.dump_all:
            dex.dump_header(options)
        if options.dump_map_list or options.dump_all:
            dex.dump_map_list(options)
        if options.dump_debug_info_items or options.dump_all:
            dex.dump_debug_info_items(options)
        if options.dump_strings or options.dump_all:
            dex.dump_string_ids(options)
        if options.dump_types or options.dump_all:
            dex.dump_type_ids(options)
        if options.dump_protos or options.dump_all:
            dex.dump_proto_ids(options)
        if options.dump_fields or options.dump_all:
            dex.dump_field_ids(options)
        if options.dump_methods or options.dump_all:
            dex.dump_class_method_ids(options)
        if options.dump_class_list or options.dump_all:
            dex.dump_class_defs(options)
        if options.dump_call_sites or options.dump_all:
            dex.dump_call_site_ids(options)
        if options.dump_method_handles or options.dump_all:
            dex.dump_method_handle_items(options)
        if options.dump_code or options.dump_all:
            dex.dump_code(options)
        if options.dump_code_items:
            dex.dump_code_items(options)
        if options.dump_structure:
            dex.dump_structure(options)
        if (
            options.dump_stats
            or options.check_encoding
            or options.new_encoding
            or options.dump_counts
        ):
            if options.dump_stats:
                for string_item in dex.get_strings():
                    if string_item.data not in string_counts:
                        string_counts[string_item.data] = 0
                    string_counts[string_item.data] += 1
            code_bytes_inefficiently_encoded = 0
            debug_info_bytes_inefficiently_encoded = 0
            new_code_bytes_inefficiently_encoded = 0
            file_opcodes_byte_size = 0
            classes = dex.get_classes()
            used_code_item_indexes = []
            for cls in classes:
                methods = cls.get_methods()
                for method in methods:
                    opcodes_bytes_size = method.get_code_byte_size()
                    file_opcodes_byte_size += opcodes_bytes_size
                    total_opcode_byte_size += opcodes_bytes_size
                    if (
                        options.dump_stats
                        or options.check_encoding
                        or options.new_encoding
                        or options.dump_counts
                    ):
                        for dex_inst in method.get_instructions():
                            if options.dump_stats:
                                op_name = dex_inst.get_name()
                                size = dex_inst.get_num_code_units() * 2
                                if op_name not in op_name_to_size:
                                    op_name_to_size[op_name] = 0
                                op_name_to_size[op_name] += size
                            if options.dump_counts:
                                op_name = dex_inst.get_name()
                                if op_name not in op_name_to_count:
                                    op_name_to_count[op_name] = 0
                                op_name_to_count[op_name] += 1
                            if options.check_encoding:
                                code_bytes_inefficiently_encoded += (
                                    dex_inst.check_encoding()
                                )
                            if options.new_encoding:
                                new_code_bytes_inefficiently_encoded += (
                                    dex_inst.new_encoding()
                                )
                        if options.check_encoding:
                            code_item_idx = method.get_code_item_index()
                            if code_item_idx >= 0:
                                used_code_item_indexes.append(code_item_idx)
                            debug_info = method.get_debug_info()
                            if debug_info:
                                debug_info_bytes_inefficiently_encoded += (
                                    method.check_debug_info_encoding()
                                )
            if options.check_encoding:
                efficiently_encoded = True
                if code_bytes_inefficiently_encoded > 0:
                    efficiently_encoded = False
                    total_code_bytes_inefficiently_encoded += (
                        code_bytes_inefficiently_encoded
                    )
                    print_code_stats(
                        code_bytes_inefficiently_encoded,
                        file_opcodes_byte_size,
                        file_size,
                    )
                if debug_info_bytes_inefficiently_encoded > 0:
                    efficiently_encoded = False
                    total_debug_info_bytes_inefficiently_encoded += (
                        debug_info_bytes_inefficiently_encoded
                    )
                    print_debug_stats(debug_info_bytes_inefficiently_encoded, file_size)
                # Verify that all code items are used.
                used_code_item_indexes.sort()
                prev_ci_idx = 0
                for ci_idx in used_code_item_indexes:
                    if ci_idx != prev_ci_idx:
                        efficiently_encoded = False
                        for idx in range(prev_ci_idx + 1, ci_idx):
                            print(
                                "code_item[%u] is not used and its "
                                "code_item can be removed" % (idx)
                            )
                    prev_ci_idx = ci_idx
                if efficiently_encoded:
                    print("file is efficiently encoded.")
            if options.new_encoding:
                if new_code_bytes_inefficiently_encoded > 0:
                    total_new_code_bytes_inefficiently_encoded += (
                        new_code_bytes_inefficiently_encoded
                    )
                    print_encoding_stats(
                        new_code_bytes_inefficiently_encoded,
                        file_opcodes_byte_size,
                        file_size,
                    )
                else:
                    print("file is efficiently encoded.")
        if options.code_duplication:
            dex.report_code_duplication()

    if options.dump_stats:
        duped_strings_byte_size = 0
        for s in string_counts:
            count = string_counts[s]
            if count > 1:
                s_len = len(s)
                duped_strings_byte_size += (count - 1) * s_len + get_uleb128_byte_size(
                    s_len
                )
        if duped_strings_byte_size > 0:
            print(
                "%u bytes in duplicated strings across dex files."
                % (duped_strings_byte_size)
            )

        print("BYTESIZE %AGE  OPCODE")
        print("======== ===== =================================")
        sorted_x = sorted(op_name_to_size.items(), key=operator.itemgetter(1))
        for (op_name, byte_size) in sorted_x:
            percentage = get_percentage(byte_size, total_opcode_byte_size)
            print("%-8u %5.2f %s" % (byte_size, percentage, op_name))
        print("-------- ----- ---------------------------------")
        print("%-8u 100.0" % (total_opcode_byte_size))

    if options.dump_counts:
        print("COUNT    OPCODE")
        print("======== =================================")
        for op_name, count in op_name_to_count.items():
            print("%-8u %s" % (count, op_name))

    if i > 0:
        if options.check_encoding:
            if total_code_bytes_inefficiently_encoded > 0:
                print_code_stats(
                    total_code_bytes_inefficiently_encoded,
                    total_opcode_byte_size,
                    total_file_size,
                )
            if total_debug_info_bytes_inefficiently_encoded > 0:
                efficiently_encoded = False
                print_debug_stats(
                    total_debug_info_bytes_inefficiently_encoded, total_file_size
                )
        if options.new_encoding:
            invoke_kind_percentage = get_percentage(
                can_use_new_encoding, can_use_new_encoding + cant_use_new_encoding
            )
            print(
                "%u invoke-kind opcodes could use new encoding"
                % (can_use_new_encoding),
                end="",
            )
            print(
                "%u could not (%2.2f%%)"
                % (cant_use_new_encoding, invoke_kind_percentage)
            )
            if total_new_code_bytes_inefficiently_encoded > 0:
                print_encoding_stats(
                    total_new_code_bytes_inefficiently_encoded,
                    total_opcode_byte_size,
                    total_file_size,
                )


if __name__ == "__main__":
    main()
