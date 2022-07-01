#! /usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import binascii
import re
import string
import struct
import sys
from io import BytesIO, StringIO


SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2


def dump_memory(base_addr, data, num_per_line, outfile):

    data_len = len(data)
    hex_string = binascii.hexlify(data)
    addr = base_addr
    ascii_str = ""
    i = 0
    concat = None
    if data_len > 0:
        if isinstance(hex_string[0], int):
            concat = lambda a, b: chr(a) + chr(b)  # noqa: E731
        else:
            concat = lambda a, b: a + b  # noqa: E731
    while i < data_len:
        outfile.write("0x%8.8x: " % (addr + i))
        bytes_left = data_len - i
        if bytes_left >= num_per_line:
            curr_data_len = num_per_line
        else:
            curr_data_len = bytes_left
        hex_start_idx = i * 2
        hex_end_idx = hex_start_idx + curr_data_len * 2
        curr_hex_str = hex_string[hex_start_idx:hex_end_idx]
        # 'curr_hex_str' now contains the hex byte string for the
        # current line with no spaces between bytes
        t = iter(curr_hex_str)
        # Print hex bytes separated by space
        outfile.write(" ".join(concat(a, b) for a, b in zip(t, t)))
        # Print two spaces
        outfile.write("  ")
        # Calculate ASCII string for bytes into 'ascii_str'
        ascii_str = ""
        for j in range(i, i + curr_data_len):
            ch = data[j]
            if isinstance(ch, int):
                ch = chr(ch)
            if ch in string.printable and ch not in string.whitespace:
                ascii_str += "%c" % (ch)
            else:
                ascii_str += "."
        # Print ASCII representation and newline
        outfile.write(ascii_str)
        i = i + curr_data_len
    outfile.write("\n")


def last_char_is_newline(s):
    if s:
        return s[-1] == "\n"
    return False


def hex_escape(s):
    return "".join(escape(c) for c in s)


def escape(c):
    if isinstance(c, int):
        c = chr(c)
    if c in string.printable:
        if c == "\n":
            return "\\n"
        if c == "\t":
            return "\\t"
        if c == "\r":
            return "\\r"
        return c
    c = ord(c)
    if c <= 0xFF:
        return "\\x" + "%02.2x" % (c)
    elif c <= "\uffff":
        return "\\u" + "%04.4x" % (c)
    else:
        return "\\U" + "%08.8x" % (c)


class FileEncode:
    """Encode binary data to a file"""

    def __init__(self, f, b="=", addr_size=0):
        """Initialize with an open binary file and optional byte order and
        address byte size.
        """
        self.file = f
        self.addr_size = addr_size
        self.set_byte_order(b)

    def align_to(self, align):
        curr_pos = self.file.tell()
        delta = curr_pos % align
        if delta:
            pad = align - delta
            if pad != 0:
                self.seek(pad, SEEK_CUR)

    def seek(self, offset, whence=SEEK_SET):
        if self.file:
            return self.file.seek(offset, whence)
        raise ValueError

    def tell(self):
        if self.file:
            return self.file.tell()
        raise ValueError

    def set_byte_order(self, b):
        '''Set the byte order, valid values are "big", "little", "swap",
        "native", "<", ">", "@", "="'''
        if b == "big":
            self.byte_order = ">"
        elif b == "little":
            self.byte_order = "<"
        elif b == "swap":
            # swap what ever the current byte order is
            if struct.pack("H", 1).startswith("\x00"):
                self.byte_order = "<"
            else:
                self.byte_order = ">"
        elif b == "native":
            self.byte_order = "="
        elif b == "<" or b == ">" or b == "@" or b == "=":
            self.byte_order = b
        else:
            raise ValueError("Invalid byte order specified: '%s'" % (b))

    def put_c_string(self, value):
        self.file.write(value)
        self.put_sint8(0)

    def put_sint8(self, value):
        """Encode a int8_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "b", value))

    def put_uint8(self, value):
        """Encode a uint8_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "B", value))

    def put_sint16(self, value):
        """Encode a int16_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "h", value))

    def put_uint16(self, value):
        """Encode a uint16_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "H", value))

    def put_sint32(self, value):
        """Encode a int32_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "i", value))

    def put_uint32(self, value):
        """Encode a uint32_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "I", value))

    def put_sint64(self, value):
        """Encode a int64_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "q", value))

    def put_uint64(self, value):
        """Encode a uint64_t into the file at the current file position"""
        self.file.write(struct.pack(self.byte_order + "Q", value))

    def put_uleb128(self, value):
        """Encode a ULEB128 into the file at the current file position"""
        while value >= 0x80:
            self.put_uint8(0x80 | (value & 0x7F))
            value >>= 7
        self.put_uint8(value)

    def put_sleb128(self, value):
        if value < 0:
            uvalue = (1 - value) * 2
        else:
            uvalue = value * 2
        while True:
            byte = value & 0x7F
            value >>= 7
            uvalue >>= 7
            if uvalue != 0:
                byte = byte | 0x80
            self.put_uint8(byte)
            if uvalue == 0:
                break

    def put_address(self, value):
        if self.addr_size == 0:
            raise ValueError
        self.put_uint_size(self.addr_size, value)

    def put_uint_size(self, size, value):
        """Encode a unsigned integer into the file at the current file
        position as an integer whose byte size is "size"."""
        if size == 1:
            return self.put_uint8(value)
        if size == 2:
            return self.put_uint16(value)
        if size == 4:
            return self.put_uint32(value)
        if size == 8:
            return self.put_uint64(value)
        print("error: integers of size %u are not supported" % (size))

    def fixup_uint_size(self, size, value, offset):
        """Fixup one unsigned integer in the file at "offset" bytes from
        the start of the file. The current file position will be saved and
        restored."""
        saved_offset = self.file.tell()
        self.file.seek(offset)
        self.put_uint_size(size, value)
        self.file.seek(saved_offset)


class FileExtract:
    """Decode binary data from a file"""

    def __init__(self, f, b="=", addr_size=0):
        """Initialize with an open binary file and optional byte order and
        address byte size
        """
        self.file = f
        self.offsets = []
        self.addr_size = addr_size
        self.set_byte_order(b)

    def get_size(self):
        pos = self.file.tell()
        self.file.seek(0, SEEK_END)
        len = self.file.tell()
        self.file.seek(pos, SEEK_SET)
        return len

    def align_to(self, align):
        curr_pos = self.file.tell()
        delta = curr_pos % align
        if delta:
            pad = align - delta
            if pad != 0:
                self.seek(pad, SEEK_CUR)

    def get_addr_size(self):
        return self.addr_size

    def set_addr_size(self, addr_size):
        self.addr_size = addr_size

    def set_byte_order(self, b):
        '''Set the byte order, valid values are "big", "little", "swap",
        "native", "<", ">", "@", "="'''
        if b == "big":
            self.byte_order = ">"
        elif b == "little":
            self.byte_order = "<"
        elif b == "swap":
            # swap what ever the current byte order is
            if struct.pack("H", 1).startswith("\x00"):
                self.byte_order = "<"
            else:
                self.byte_order = ">"
        elif b == "native":
            self.byte_order = "="
        elif b == "<" or b == ">" or b == "@" or b == "=":
            self.byte_order = b
        else:
            print("Invalid byte order specified: '%s'" % (b))

    def seek(self, offset, whence=SEEK_SET):
        if self.file:
            return self.file.seek(offset, whence)
        raise ValueError

    def tell(self):
        if self.file:
            return self.file.tell()
        raise ValueError

    def read_data(self, byte_size):
        bytes = self.read_size(byte_size)
        if len(bytes) == byte_size:
            return FileExtract(
                StringIO(bytes.decode("utf-8")), self.byte_order, self.addr_size
            )
        return None

    def read_size(self, byte_size):
        s = self.file.read(byte_size)
        if len(s) != byte_size:
            return None
        return s

    def push_offset_and_seek(self, offset, whence=SEEK_SET):
        '''Push the current file offset and seek to "offset"'''
        self.offsets.append(self.file.tell())
        self.file.seek(offset, whence)

    def pop_offset_and_seek(self):
        """Pop a previously pushed file offset and set the file position."""
        if len(self.offsets) > 0:
            self.file.seek(self.offsets.pop(), SEEK_SET)

    def get_sint8(self, fail_value=0):
        """Extract a int8_t from the current file position."""
        s = self.read_size(1)
        return self._unpack("b", s) if s else fail_value

    def get_uint8(self, fail_value=0):
        """Extract and return a uint8_t from the current file position."""
        s = self.read_size(1)
        return self._unpack("B", s) if s else fail_value

    def get_sint16(self, fail_value=0):
        """Extract a int16_t from the current file position."""
        s = self.read_size(2)
        return self._unpack("h", s) if s else fail_value

    def get_uint16(self, fail_value=0):
        """Extract a uint16_t from the current file position."""
        s = self.read_size(2)
        return self._unpack("H", s) if s else fail_value

    def get_sint32(self, fail_value=0):
        """Extract a int32_t from the current file position."""
        s = self.read_size(4)
        return self._unpack("i", s) if s else fail_value

    def get_uint32(self, fail_value=0):
        """Extract a uint32_t from the current file position."""
        s = self.read_size(4)
        return self._unpack("I", s) if s else fail_value

    def get_sint64(self, fail_value=0):
        """Extract a int64_t from the current file position."""
        s = self.read_size(8)
        return self._unpack("q", s) if s else fail_value

    def get_uint64(self, fail_value=0):
        """Extract a uint64_t from the current file position."""
        s = self.read_size(8)
        return self._unpack("Q", s) if s else fail_value

    def _unpack(self, format_suffix, s):
        return struct.unpack(self.byte_order + format_suffix, s)[0]

    def get_address(self, fail_value=0):
        if self.addr_size == 0:
            print("error: invalid addr size...")
            raise ValueError
        else:
            return self.get_uint_size(self.addr_size, fail_value)

    def get_sint_size(self, size, fail_value=0):
        """Extract a signed integer from the current file position whose
        size is "size" bytes long."""
        if size == 1:
            return self.get_sint8(fail_value)
        if size == 2:
            return self.get_sint16(fail_value)
        if size == 4:
            return self.get_sint32(fail_value)
        if size == 8:
            return self.get_sint64(fail_value)
        print("error: integer of size %u is not supported" % (size))
        return fail_value

    def get_uint_size(self, size, fail_value=0):
        """Extract a unsigned integer from the current file position whose
        size is "size" bytes long."""
        if size == 1:
            return self.get_uint8(fail_value)
        if size == 2:
            return self.get_uint16(fail_value)
        if size == 4:
            return self.get_uint32(fail_value)
        if size == 8:
            return self.get_uint64(fail_value)
        print("error: integer of size %u is not supported" % (size))
        return fail_value

    def get_fixed_length_c_string(
        self, n, fail_value="", isprint_only_with_space_padding=False
    ):
        """Extract a fixed length C string from the current file position."""
        s = self.read_size(n)
        if s:
            (cstr,) = struct.unpack(self.byte_order + ("%i" % n) + "s", s)
            # Strip trialing NULLs
            cstr = cstr.strip(b"\0")
            if isprint_only_with_space_padding:
                for c in cstr:
                    if c in string.printable or ord(c) == 0:
                        continue
                    return fail_value
            return cstr
        else:
            return fail_value

    def get_c_string(self):
        """Extract a NULL terminated C string from the current position."""
        cstr = ""
        byte = self.get_uint8()
        while byte != 0:
            cstr += "%c" % byte
            byte = self.get_uint8()
        return cstr

    def get_n_sint8(self, n, fail_value=0):
        """Extract "n" int8_t values from the current position as a list."""
        s = self.read_size(n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "b", s)
        else:
            return (fail_value,) * n

    def get_n_uint8(self, n, fail_value=0):
        """Extract "n" uint8_t values from the current position as a list."""
        s = self.read_size(n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "B", s)
        else:
            return (fail_value,) * n

    def get_n_sint16(self, n, fail_value=0):
        """Extract "n" int16_t values from the current position as a list."""
        s = self.read_size(2 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "h", s)
        else:
            return (fail_value,) * n

    def get_n_uint16(self, n, fail_value=0):
        """Extract "n" uint16_t values from the current position as a list."""
        s = self.read_size(2 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "H", s)
        else:
            return (fail_value,) * n

    def get_n_sint32(self, n, fail_value=0):
        """Extract "n" int32_t values from the current position as a list."""
        s = self.read_size(4 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "i", s)
        else:
            return (fail_value,) * n

    def get_n_uint32(self, n, fail_value=0):
        """Extract "n" uint32_t values from the current position as a list."""
        s = self.read_size(4 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "I", s)
        else:
            return (fail_value,) * n

    def get_n_sint64(self, n, fail_value=0):
        """Extract "n" int64_t values from the current position as a list."""
        s = self.read_size(8 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "q", s)
        else:
            return (fail_value,) * n

    def get_n_uint64(self, n, fail_value=0):
        """Extract "n" uint64_t values from the current position as a list."""
        s = self.read_size(8 * n)
        if s:
            return struct.unpack(self.byte_order + ("%u" % n) + "Q", s)
        else:
            return (fail_value,) * n

    def get_uleb128p1(self, fail_value=0):
        return self.get_uleb128(fail_value) - 1

    def get_uleb128(self, fail_value=0):
        """Extract a ULEB128 number"""
        byte = self.get_uint8()
        # Quick test for single byte ULEB
        if byte & 0x80:
            result = byte & 0x7F
            shift = 7
            while byte & 0x80:
                byte = self.get_uint8()
                result |= (byte & 0x7F) << shift
                shift += 7
            return result
        else:
            return byte  # Simple one byte ULEB128 value...

    def get_sleb128(self, fail_value=0):
        result = 0
        shift = 0
        size = 64
        byte = 0
        bytecount = 0
        while 1:
            bytecount += 1
            byte = self.get_uint8()
            result |= (byte & 0x7F) << shift
            shift += 7
            if (byte & 0x80) == 0:
                break
        # Sign bit of byte is 2nd high order bit (0x40)
        if shift < size and (byte & 0x40):
            result |= -(1 << shift)
        return result

    def dump(self, start=0, end=-1):
        if end == -1:
            self.seek(start, SEEK_END)  # Seek to end to get size
            n = self.tell() - start
        else:
            n = end - start
        self.seek(start, SEEK_SET)
        bytes = self.read_size(n)
        dump_memory(0, bytes, 32, sys.stdout)


def main():
    uleb_tests = [
        (struct.pack("B", 0x02), 2),
        (struct.pack("B", 0x7F), 127),
        (struct.pack("2B", 0x80, 0x01), 128),
        (struct.pack("2B", 0x81, 0x01), 129),
        (struct.pack("2B", 0x82, 0x01), 130),
        (struct.pack("2B", 0xB9, 0x64), 12857),
    ]

    sleb_tests = [
        (struct.pack("B", 0x02), 2),
        (struct.pack("B", 0x7E), -2),
        (struct.pack("2B", 0xFF, 0x00), 127),
        (struct.pack("2B", 0x81, 0x7F), -127),
        (struct.pack("2B", 0x80, 0x01), 128),
        (struct.pack("2B", 0x80, 0x7F), -128),
        (struct.pack("2B", 0x81, 0x01), 129),
        (struct.pack("2B", 0xFF, 0x7E), -129),
    ]
    num_errors = 0
    print("Running unit tests...", end="")
    for (s, check_n) in sleb_tests:
        e = FileExtract(BytesIO(s))
        n = e.get_sleb128()
        if n != check_n:
            num_errors += 1
            print("\nerror: sleb128 extraction failed for %i (got %i)" % (check_n, n))
            dump_memory(0, s, 32, sys.stdout)
    for (s, check_n) in uleb_tests:
        e = FileExtract(BytesIO(s))
        n = e.get_uleb128()
        if n != check_n:
            num_errors += 1
            print("\nerror: uleb128 extraction failed for %i (got %i)" % (check_n, n))
            dump_memory(0, s, 32, sys.stdout)
    if num_errors == 0:
        print("ok")
    else:
        print("%u errors" % (num_errors))
    print


if __name__ == "__main__":
    main()


class AutoParser:
    """A class that enables easy parsing of binary files.

    This class is designed to be sublcassed and clients must provide a list of
    items in the constructor. Each item in the items list is a dictionary that
    describes each attribute that should be added to the class when it is
    decoded. A quick example for a C structure:

        struct load_command {
                uint32_t cmd;           /* type of load command */
                uint32_t cmdsize;       /* total size of command in bytes */
        };

    The python code would look like:

        class load_command(file_extract.AutoParser):
            items = [
                { 'name':'cmd', 'type':'u32' },
                { 'name':'cmdsize', 'type':'u32'},
            ]
            def __init__(self, data):
                AutoParser.__init__(self, self.items, data)

    Decoding a single load_command from a file involves opening a file and
    creating a FileExtract object, and then decoding the load_command object:

        file = open(path)
        data = file_extract.FileExtract(file, '=', 4)
        lc = load_command(data)

    The 'lc' object now has two properties:

        lc.cmd
        lc.cmdsize

    Item dictionaries are very easy to define and have quite a many options
    to ensure it is very easy to parse a binary file by defining many
    subclasses of file_extract.AutoParser and combining them together.

    Item dictionaries can contain the following keys:
    KEY NAME       DESCRIPTION
    ============== ============================================================
    'name'         A string name of the attribute to add to this class when
                   decoding. If an item has no name, it will not be added to
                   this object when it is being decoded. Omitting the name is
                   handy when you have padding where you might need to decode
                   some bytes that are part of the on disk representation of
                   the binary object, but don't need the value represented
                   in the object itself.
    'type'         A string name for the type of the data to decode. See
                   "Builin Types" table below for valid typename values. Either
    'class'        An AutoParser sublcass class that will be used to decode
                   this item by constructing it with the data at the current
                   offset. This allows you to compose a AutoParser object
                   that is contained within another AutoParser object.
    'condition'    A function that takes two arguments: the current AutoParser
                   object that is in the process of being decoded and the
                   FileExtract object. The function returns True if this item
                   is present and should be decoded, and False if it should be
                   skipped. The condition is evaluated before the value is
                   decoded and stops the type/class/decode from decoding the
                   object. This can be used to only decode a value if a
                   previous attribute is a specific value. If a 'default' key
                   is present in the item dictionary, then the 'default' value
                   will be set as the the value for this item, otherwise the
                   attribute will not be added to this object:
                       condition_passed = item['condition'](AutoParser,
                                                            FileExtract)
    'default'      The default value for the current item that will be set if
                   the 'condition' callback function returns False.
    'decode'       A function that take a single file_extract.FileExtract
                   object argument and returns the value for this item.
                       value = item['decode'](FileExtract)
    'align'        An integer that gives the file offset alignment for this
                   item. This alignment can be any number and the file
                   position will be advanced to the next aligned offset if
                   needed prior to reading the value
    'attr_count'   A string that specifies the name of an attribute that has
                   already been decoded in this object. This indicates that the
                   value for this item is a list whose size is the integer
                   value of the attribute that was already decoded in a
                   previous item in this object.
    'attr_offset'  An integer that this item's value is contained within the
                   file at the specified offset. A seek will be performed on
                   the file before reading the value of this object. The file
                   position will be pushed onto a stack, a seek will be
                   performed, the item's value will be read, and then the file
                   position will be restored.
    'attr_offset_size' A string name of an existing attribute that contains
                   the end offset of the data for this object. This is useful
                   when a list of items is contained in the file and the count
                   of the items is not specified, just the end offset. This is
                   often used with the 'attr_offset' key/value pair. The
                   type/class/decode will be continually called until the file
                   offset exceeds the offset + 'attr_offset_size'. String
                   tables are good example of when this is used as they string
                   table offset and size are often specified, but no the
                   number of strings in the string table.
    'attr_offset_whence' A string name that specifies the type of seek to
                   perform on the 'attr_offset' value. This can be one of
                   "item", "file", "eof", "curr". "item" specifies the offset
                   is relative to the starting offset of this object. "file"
                   specifies that the offset is relative to the start of the
                   file. "eof" specifies that the offset is relative to the
                   end of tile. "curr" specifies that the offset is relative
                   to the current file position.
    'validate'     A function pointer that will be called after the value has
                   been extracted. The function is called with the extracted
                   value and should return None if the value is valid, or
                   return an error string if the value is not valid:
                       error = item['validate'](value)
                       if error:
                           raise ValueError(error)
    'value_fixup'  A function pointer that will be called after the item's
                   value has been decoded. The function will be called with one
                   argument, the decoded value, and returns the fixed value:
                       value = item['value_fixup'](value)
    'debug'        A string value that is printed prior to decoding the item's
                   value. The printed string value is prefixed by the current
                   file offset and allows debugging of where a value is being
                   decoded within the file. This helps debug the decoding of
                   items.
    'switch'       The string name of an attribute that was already decoded in
                   this object. The attribute value will be used as a key into
                   the 'cases' item key/value pair in the items supplied to the
                   AutoParser object. If the attribute value is not found in
                   the 'cases' dictionary, then 'default' will be used as the
                   key into the 'cases' dictionary. See 'cases' below. See
                   "Switch Example" below for more information.
    'cases'        A dictionary of values to items arrays. The 'switch' key
                   above specifies the name of an attribute in this object that
                   will be used as the key into the dictionary specified in
                   this key/value pair. The items that are found during the
                   lookup will then be decoded into this object. See
                   "Switch Example" below for more information.
    'dump'         A function pointer that is called to dump the value. The
                   function gets called with the value and the file:
                        def dump(value, file):
                            ...
    'dump_list'    A function pointer that is called to dump a list of values.
                   The function gets called with the value and the file:
                        def dump_list(value, prefix, flat, file):
                            ...
    EXAMPLE 1

    If you have a structure that has a count followed by an array of items
    whose size is the value of count:

        struct NumberArray {
            uint32_t count;
            uint32_t numbers[];
        };

    This would be respresented by the following items:

    class NumberArray(AutoParser):
        items = [
            {'type':'u32', 'name':'count'},
            {'type':'u32', 'name':'numbers', 'attr_count' : 'count'},
        ]
        def __init__(self, data):
            AutoParser.__init__(self, self.items, data)

    The second item named 'numbers' will be decoded as a list of 'obj.count'
    u32 values as the 'attr_count' specifies the name of an attribute that
    has already been decoded into the object 'obj' and contains the count.

    EXAMPLE 2

    Sometimes a structure contains an offset and a count of objects. In the
    example below SymtabInfo contains the offset and count of Symbol objects
    that appear later in the file:
        struct SymtabInfo {
            uint32_t symtab_offset;
            uint32_t num_symbols;
        }
        struct Symbol {
            ...;
        };

    The symbol table can be decoded by combinging the two things together
    into the same object when decoding:

       class Symbol(AutoParser):
           ...
       class SymtabInfo(AutoParser):
           items = [
                {'type' : 'u32', 'name' : 'symtab_offset'},
                {'type' : 'u32', 'name' : 'num_symbols' },
                {'class' : Symbol,
                 'name' : 'symbols',
                 'attr_offset' : 'symtab_offset',
                 'attr_count' : 'num_symbols' }
            ]
            def __init__(self, data):
                AutoParser.__init__(self, self.items, data)

    """

    type_regex = re.compile(r"([^\[]+)\[([0-9]+)\]")
    default_formats = {
        "u8": "%#2.2x",
        "u16": "%#4.4x",
        "u32": "%#8.8x",
        "u64": "%#16.16x",
        "addr": "%#16.16x",
        "cstr": '"%s"',
    }
    read_value_callbacks = {
        "u8": lambda data: data.get_uint8(),
        "u16": lambda data: data.get_uint16(),
        "u32": lambda data: data.get_uint32(),
        "u64": lambda data: data.get_uint64(),
        "s8": lambda data: data.get_sint8(),
        "s16": lambda data: data.get_sint16(),
        "s32": lambda data: data.get_sint32(),
        "s64": lambda data: data.get_sint64(),
        "addr": lambda data: data.get_address(),
        "uleb": lambda data: data.get_uleb128(),
        "sleb": lambda data: data.get_sleb128(),
        "ulebp1": lambda data: data.get_uleb128p1(),
    }

    def __init__(self, items, data, context=None):
        self.__offset = data.tell()
        self.items = items
        self.context = context  # Any object you want to store for future usage
        self.max_name_len = 0
        self.extract_items(items, data)
        self.__len = data.tell() - self.__offset

    def __len__(self):
        return self.__len

    def get_list_header_lines(self):
        """When an object of this type is in a list, print out this string
        before printing out any items"""
        return None

    def get_dump_header(self):
        """Override in subclasses to print this string out before any items
        are dumped. This is a good place to put a description of the item
        represented by this class and possible to print out a table header
        in case the items are a list"""
        return None

    def get_dump_prefix(self):
        """Override in subclasses to print out a string before each item in
        this class"""
        return None

    def get_dump_flat(self):
        return False

    def get_offset(self):
        return self.__offset

    def extract_items(self, items, data):
        for item in items:
            offset_pushed = False
            if "attr_offset" in item:
                offset = getattr(self, item["attr_offset"])
                if "attr_offset_whence" in item:
                    offset_base = item["attr_offset_whence"]
                    if offset_base == "item":
                        # Offset from the start of this item
                        data.push_offset_and_seek(offset + self.get_offset())
                        offset_pushed = True
                    elif offset_base == "file":
                        # Offset from the start of the file
                        data.push_offset_and_seek(offset, SEEK_SET)
                        offset_pushed = True
                    elif offset_base == "eof":
                        # Offset from the end of the file
                        data.push_offset_and_seek(offset, SEEK_END)
                        offset_pushed = True
                    elif offset_base == "curr":
                        # Offset from the current file position
                        data.push_offset_and_seek(offset, SEEK_CUR)
                        offset_pushed = True
                    else:
                        raise ValueError(
                            '"attr_offset_whence" can be one of "item", '
                            '"file", "eof", "curr" (defaults to "file")'
                        )
                else:
                    # Default to offset from the start of the file
                    data.push_offset_and_seek(offset, SEEK_SET)
                    offset_pushed = True
            if "debug" in item:
                print("%#8.8x: %s" % (self.__offset, item["debug"]))
                continue
            if "switch" in item:
                if "cases" not in item:
                    raise ValueError(
                        'items with a "switch" key/value pair, '
                        'must have a "cases" key/value pair'
                    )
                cases = item["cases"]
                switch_value = getattr(self, item["switch"])
                if switch_value in cases:
                    case_items = cases[switch_value]
                elif "default" in cases:
                    case_items = cases["default"]
                else:
                    raise ValueError("unhandled switch value %s" % (str(switch_value)))
                self.extract_items(case_items, data)
                continue

            # Check if this item is just an alignment directive?
            condition_passed = True
            if "condition" in item:
                condition_passed = item["condition"](self, data)
            if "align" in item:
                if condition_passed:
                    data.align_to(item["align"])
            count = self.read_count_from_item(item)
            value_fixup = None
            # If there is a value fixup key, then call the function with the
            # data and the value. The return value will be a fixed up value
            # and the function also has the ability to modify the data stream
            # (set the byte order, address byte size, etc).
            if "value_fixup" in item:
                value_fixup = item["value_fixup"]

            if "attr_offset_size" in item:
                # the number of items is inferred by parsing up until
                # attr_offset + attr_offset_size, so we create a new
                # FileExtract object that only contains the data we need and
                # extract using that data.
                attr_offset_size = getattr(self, item["attr_offset_size"])
                item_data = data.read_data(attr_offset_size)
                if item_data is None:
                    raise ValueError("failed to get item data")
                value = self.decode_value(
                    item_data, item, condition_passed, value_fixup
                )
            else:
                if count is None:
                    value = self.decode_value(data, item, condition_passed, value_fixup)
                else:
                    value = []
                    for _ in range(count):
                        value.append(
                            self.decode_value(data, item, condition_passed, value_fixup)
                        )

            if "validate" in item:
                error = item["validate"](value)
                if error is not None:
                    raise ValueError("error: %s" % (error))
            if "name" in item and value is not None:
                name = item["name"]
                setattr(self, name, value)
                name_len = len(name)
                if self.max_name_len < name_len:
                    self.max_name_len = name_len
            if offset_pushed:
                data.pop_offset_and_seek()

    def decode_value(self, data, item, condition_passed, value_fixup):
        # If the item has a 'condition' key, then this is a function
        # that we pass "self" to in order to determine if this value
        # is available. If the callback returns False, then we set the
        # value to the default value
        read_value = True
        if not condition_passed:
            if "default" in item:
                v = item["default"]
            else:
                v = None
            read_value = False

        if read_value:
            if "type" in item:
                v = self.read_type(data, item)
            elif "class" in item:
                v = item["class"](data)
            elif "decode" in item:
                v = item["decode"](data)
            else:
                raise ValueError(
                    'item definitions must have a "type" or '
                    '"class" or "decode" field'
                )
            # Let the item fixup each value if needed and possibly
            # adjust the byte size or byte order.
            if value_fixup is not None:
                v = value_fixup(data, v)
        return v

    def dump_item(self, prefix, f, item, print_name, parent_path, flat):
        if "switch" in item:
            cases = item["cases"]
            switch_value = getattr(self, item["switch"])
            if switch_value in cases:
                case_items = cases[switch_value]
            elif "default" in cases:
                case_items = cases["default"]
            for case_item in case_items:
                self.dump_item(prefix, f, case_item, print_name, parent_path, flat)
            return
        # We skip printing an item if any of the following are true:
        # - If there is no name (padding)
        # - If there is a 'dump' value key/value pair with False as the value
        if "name" not in item or "dump" in item and item["dump"] is False:
            return
        name = item["name"]
        if not hasattr(self, name):
            return
        value = getattr(self, name)
        value_is_list = type(value) is list
        # If flat is None set its value automatically
        if flat is None:
            flat = self.get_dump_flat()
            if value_is_list:
                if "table_header" in item:
                    table_header = item["table_header"]
                    f.write(table_header)
                    if not last_char_is_newline(table_header):
                        f.write("\n")
                    print_name = False
                    flat = True
        if prefix is None:
            prefix = self.get_dump_prefix()
        flat_list = value_is_list and "flat" in item and item["flat"]
        if prefix and flat_list is False:
            f.write(prefix)
        if print_name:
            if not flat_list:
                if flat:
                    f.write(name)
                    f.write("=")
                else:
                    f.write("%-*s" % (self.max_name_len, name))
                    f.write(" = ")
        if "dump" in item:
            item["dump"](value, f)
            return
        elif "dump_list" in item:
            item["dump_list"](value, prefix, flat, f)
            return
        else:
            if value_is_list:
                if parent_path is None:
                    item_path = name
                else:
                    item_path = parent_path + "." + name
                self.dump_values(f, item, value, print_name, item_path, prefix)
            else:
                if "dump_width" in item:
                    dump_width = item["dump_width"]
                    strm = StringIO()
                    self.dump_value(strm, item, value, print_name, parent_path)
                    s = strm.getvalue()
                    f.write(s)
                    s_len = len(s)
                    if s_len < dump_width:
                        f.write(" " * (dump_width - s_len))
                else:
                    self.dump_value(f, item, value, print_name, parent_path)
        if not flat_list:
            if flat:
                f.write(" ")
            else:
                f.write("\n")

    def dump_value(self, f, item, value, print_name, parent_path):
        if value is None:
            f.write("<NULL>")
            return
        if "stringify" in item:
            f.write("%s" % item["stringify"](value))
            return
        if "type" in item:
            itemtype = item["type"]
            if "format" in item:
                format = item["format"]
            elif itemtype in self.default_formats:
                format = self.default_formats[itemtype]
            else:
                format = None
            if format:
                f.write(format % (value))
            else:
                if itemtype.startswith("cstr"):
                    f.write('"')
                    f.write(hex_escape(value))
                    f.write('"')
                else:
                    f.write(str(value))
        elif "class" in item:
            value.dump(prefix=None, print_name=print_name, f=f, parent_path=parent_path)
        else:
            raise ValueError(
                "item's with names must have a 'type' or " "'class' key/value pair"
            )

    def dump_values(self, f, item, values, print_name, parent_path, prefix):
        if len(values) == 0:
            if "flat" in item and item["flat"]:
                if prefix:
                    f.write(prefix)
                if parent_path:
                    f.write(parent_path)
            f.write("[]\n")
            return
        flat = self.get_dump_flat()
        if flat is False and "flat" in item:
            flat = item["flat"]
        count = len(values)
        if count > 0:
            index_width = 1
            w = count
            while w > 10:
                index_width += 1
                w /= 10
            if isinstance(values[0], AutoParser):
                first = values[0]
                table_header_lines = first.get_list_header_lines()
                if table_header_lines:
                    f.write("\n")
                    print_name = False
                    flat = True
                    for line in table_header_lines:
                        f.write(" " * (index_width + 3))
                        f.write(line)
            index_format = "[%%%uu]" % (index_width)
            if prefix is None:
                prefix = ""
            for (i, value) in enumerate(values):
                if flat:
                    if prefix:
                        f.write(prefix)
                    if parent_path:
                        f.write(parent_path)
                    f.write(index_format % (i))
                    f.write(" = ")
                else:
                    format = "\n%s%s" + index_format + "\n"
                    f.write(format % (prefix, parent_path, i))
                self.dump_value(f, item, value, print_name, parent_path)
                f.write("\n")

    def dump(
        self, prefix=None, f=sys.stdout, print_name=True, parent_path=None, flat=None
    ):
        header = self.get_dump_header()
        if header:
            f.write(header)
            if not last_char_is_newline(header):
                f.write("\n")
        for item in self.items:
            self.dump_item(prefix, f, item, print_name, parent_path, flat)

    def read_count_from_item(self, item):
        if "attr_count" in item:
            # If 'attr_count' is in the dictionary. If so, it means that
            # there is already an attribute in this object that has the
            # count in it and we should ready that many of the type
            count = getattr(self, item["attr_count"])
            # If there is an 'attr_count_fixup' key, it is a function that
            # will fixup the count value
            if "attr_count_fixup" in item:
                count = item["attr_count_fixup"](count)
            return count
        elif "count" in item:
            return item["count"]
        return None

    def read_builtin_type(self, data, typename, item):
        if typename in self.read_value_callbacks:
            return self.read_value_callbacks[typename](data)
        if typename == "cstr":
            count = self.read_count_from_item(item)
            if count is None:
                return data.get_c_string()
            else:
                return data.get_fixed_length_c_string(count)
        if typename == "bytes":
            if "attr_size" in item:
                size = getattr(self, item["attr_size"])
                return data.read_size(size)
            else:
                raise ValueError(
                    "'bytes' must have either a 'count' or a "
                    "'attr_count' key/value pair"
                )
        raise ValueError("invalid 'type' value %s" % (typename))

    def read_type(self, data, item):
        typename = item["type"]
        if "[" in typename:
            match = self.type_regex.match(typename)
            if not match:
                raise ValueError(
                    "item type array must be a valid type "
                    "followed by [] with a decimal number "
                    "as the size"
                )
            basetype = match.group(1)
            count = int(match.group(2))
            if basetype == "cstr":
                return data.get_fixed_length_c_string(count)
            result = []
            for _ in range(count):
                result.append(self.read_builtin_type(data, basetype, item))
            return result
        else:
            return self.read_builtin_type(data, typename, item)
