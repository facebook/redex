#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import re
import readline
import sqlite3
import sys
from os.path import expanduser


PAT_ANON_CLASS = re.compile(r".*\$[0-9]+;")

HISTORY_FILE = expanduser("~/.dexsql.history")

OPCODES = {}
OPCODES[0x1A] = "const-string"
OPCODES[0x1B] = "const-string/jumbo"
OPCODES[0x1C] = "const-class"
OPCODES[0x1F] = "check-cast"
OPCODES[0x20] = "instance-of"
OPCODES[0x22] = "new-instance"
OPCODES[0x52] = "iget"
OPCODES[0x53] = "iget/wide"
OPCODES[0x54] = "iget-object"
OPCODES[0x55] = "iget-boolean"
OPCODES[0x56] = "iget-byte"
OPCODES[0x57] = "iget-char"
OPCODES[0x58] = "iget-short"
OPCODES[0x59] = "iput"
OPCODES[0x5A] = "iput/wide"
OPCODES[0x5B] = "iput-object"
OPCODES[0x5C] = "iput-boolean"
OPCODES[0x5D] = "iput-byte"
OPCODES[0x5E] = "iput-char"
OPCODES[0x5F] = "iput-short"
OPCODES[0x60] = "sget"
OPCODES[0x61] = "sget/wide"
OPCODES[0x62] = "sget-object"
OPCODES[0x63] = "sget-boolean"
OPCODES[0x64] = "sget-byte"
OPCODES[0x65] = "sget-char"
OPCODES[0x66] = "sget-short"
OPCODES[0x67] = "sput"
OPCODES[0x68] = "sput/wide"
OPCODES[0x69] = "sput-object"
OPCODES[0x6A] = "sput-boolean"
OPCODES[0x6B] = "sput-byte"
OPCODES[0x6C] = "sput-char"
OPCODES[0x6D] = "sput-short"
OPCODES[0x6E] = "invoke-virtual"
OPCODES[0x6F] = "invoke-super"
OPCODES[0x70] = "invoke-direct"
OPCODES[0x71] = "invoke-static"
OPCODES[0x72] = "invoke-interface"
OPCODES[0x74] = "invoke-virtual/range"
OPCODES[0x75] = "invoke-super/range"
OPCODES[0x76] = "invoke-direct/range"
OPCODES[0x77] = "invoke-static/range"
OPCODES[0x78] = "invoke-interface/range"


# operates on classes.name column
# return the first n levels of the package: PKG("com/foo/bar", 2) => "com/foo"
def udf_pkg_2arg(text, n):
    groups = text.split("/")
    if n >= (len(groups) - 1):
        n = len(groups) - 1
    return "/".join(groups[:n])


def udf_pkg_1arg(text):
    return udf_pkg_2arg(text, 9999)


# operates on access column
def udf_is_interface(access):
    return access & 0x00000200


# operates on access column
def udf_is_static(access):
    return access & 0x00000008


# operates on access column
def udf_is_final(access):
    return access & 0x00000010


# operates on access column
def udf_is_native(access):
    return access & 0x00000100


# operates on access column
def udf_is_abstract(access):
    return access & 0x00000400


# operates on access column
def udf_is_synthetic(access):
    return access & 0x00001000


# operates on access column
def udf_is_annotation(access):
    return access & 0x00002000


# operates on access column
def udf_is_enum(access):
    return access & 0x00004000


# operates on access column
def udf_is_constructor(access):
    return access & 0x00010000


# operates on dex column
def udf_is_voltron_dex(dex_id):
    return not dex_id.startswith("dex/")


# operates on classes.name
def udf_is_inner_class(name):
    return "$" in name


# operates on classes.name
def udf_is_anon_class(name):
    return PAT_ANON_CLASS.match(name) is not None


# convert a numerical opcode to its name
def udf_opcode(opcode):
    return OPCODES.get(opcode, opcode)


# operates on dex column
def udf_is_coldstart(dex_id):
    return dex_id == "dex/0" or dex_id == "dex/1" or dex_id == "dex/2"


def udf_is_default_ctor(name):
    return name == ";.<init>:()V"


# operates on fields.name
class AggregateFieldShape:
    def __init__(self):
        self.shape = {}

    def step(self, value):
        element = value[value.index(":") + 1]
        self.shape[element] = self.shape[element] + 1 if element in self.shape else 1

    def finalize(self):
        return " ".join("%s:%s" % (k, v) for k, v in sorted(self.shape.items()))


conn = sqlite3.connect(sys.argv[1])
conn.create_function("PKG", 2, udf_pkg_2arg)
conn.create_function("PKG", 1, udf_pkg_1arg)
conn.create_function("IS_INTERFACE", 1, udf_is_interface)
conn.create_function("IS_STATIC", 1, udf_is_static)
conn.create_function("IS_FINAL", 1, udf_is_final)
conn.create_function("IS_NATIVE", 1, udf_is_native)
conn.create_function("IS_ABSTRACT", 1, udf_is_abstract)
conn.create_function("IS_SYNTHETIC", 1, udf_is_synthetic)
conn.create_function("IS_ANNOTATION", 1, udf_is_annotation)
conn.create_function("IS_ENUM", 1, udf_is_enum)
conn.create_function("IS_CONSTRUCTOR", 1, udf_is_constructor)
conn.create_function("IS_DEFAULT_CONSTRUCTOR", 1, udf_is_default_ctor)
conn.create_function("IS_VOLTRON_DEX", 1, udf_is_voltron_dex)
conn.create_function("IS_INNER_CLASS", 1, udf_is_inner_class)
conn.create_function("IS_ANON_CLASS", 1, udf_is_anon_class)
conn.create_function("OPCODE", 1, udf_opcode)
conn.create_function("IS_COLDSTART", 1, udf_is_coldstart)
conn.create_aggregate("FIELD_SHAPE", 1, AggregateFieldShape)

cursor = conn.cursor()

open(HISTORY_FILE, "a")
readline.read_history_file(HISTORY_FILE)
readline.set_history_length(1000)
while True:
    line = eval(input("> "))
    readline.write_history_file(HISTORY_FILE)
    try:
        rows = 0
        cursor.execute(line)
        for row in cursor.fetchall():
            print(str(row))
            rows += 1
        print("%d rows returned by query" % (rows))
    except sqlite3.OperationalError as e:
        print("Query caused exception: %s" % str(e))

cursor.close()
conn.close()
