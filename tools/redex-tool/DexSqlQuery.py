#!/usr/bin/env python3

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
import sqlite3
import sys
import readline
from os.path import expanduser

HISTORY_FILE = expanduser("~/.dexsql.history")

# operates on classes.name column
# return the first n levels of the package: PKG("com/foo/bar", 2) => "com/foo"
def udf_pkg_2arg(text, n):
    groups = text.split('/')
    if (n >= (len(groups) - 1)):
        n = len(groups) - 1
    return '/'.join(groups[:n])

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
def udf_is_voltron(dex_id):
    return not dex_id.startswith("dex/")

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
conn.create_function("IS_VOLTRON", 1, udf_is_voltron)

cursor = conn.cursor()

open(HISTORY_FILE, 'a')
readline.read_history_file(HISTORY_FILE)
readline.set_history_length(1000)
while True:
    line = input("> ")
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
