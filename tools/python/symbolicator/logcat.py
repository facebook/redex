# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import re


class LogcatSymbolicator(object):

    CLASS_REGEX = re.compile(r"\b[A-Za-z][0-9A-Za-z_$]*\.[0-9A-Za-z_$.]+\b")

    TRACE_REGEX = re.compile(
        r"^(?P<prefix>.*)\s+at (?P<class>[A-Za-z][0-9A-Za-z_$]*\.[0-9A-Za-z_$.]+)"
        r"\.(?P<method>[0-9A-Za-z_$<>]+)\(:(?P<lineno>\d+)\)\s*\n",
        re.MULTILINE,
    )

    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps

    def class_replacer(self, matchobj):
        m = matchobj.group(0)
        if m in self.symbol_maps.class_map:
            return self.symbol_maps.class_map[m].origin_class
        return m

    def line_replacer(self, matchobj):
        lineno = int(matchobj.group("lineno"))
        cls = matchobj.group("class")
        if self.symbol_maps.iodi_metadata is not None:
            mapped_lineno, _ = self.symbol_maps.iodi_metadata.map_iodi(
                self.symbol_maps.debug_line_map,
                cls,
                matchobj.group("method"),
                lineno,
                None,
            )
            lineno = mapped_lineno if mapped_lineno else lineno
        positions = self.symbol_maps.line_map.get_stack(lineno - 1)
        if cls in self.symbol_maps.class_map:
            cls = self.symbol_maps.class_map[cls]
        result = ""
        for pos in positions:
            if pos.method is None:
                result += "%s\tat %s.%s(%s:%d)\n" % (
                    matchobj.group("prefix"),
                    cls,
                    matchobj.group("method"),
                    pos.file,
                    pos.line,
                )
            else:
                result += "%s\tat %s(%s:%d)\n" % (
                    matchobj.group("prefix"),
                    pos.method,
                    pos.file,
                    pos.line,
                )
        return result

    def symbolicate(self, line):
        line = self.CLASS_REGEX.sub(self.class_replacer, line)
        line = self.TRACE_REGEX.sub(self.line_replacer, line)
        return line

    @staticmethod
    def is_likely_logcat(line):
        return line.startswith("--------- beginning of") or re.match(
            r"[A-Z]/[A-Za-z0-9_$](\s*\d+):", line
        )
