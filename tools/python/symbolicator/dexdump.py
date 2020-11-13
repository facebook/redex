# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import re


class DexdumpSymbolicator(object):

    CLASS_REGEX = re.compile(r"L(?P<class>[A-Za-z][0-9A-Za-z_$]*\/[0-9A-Za-z_$\/]+);")

    LINE_REGEX = re.compile(r"(?P<prefix>0x[0-9a-f]+ line=)(?P<lineno>\d+)")

    # METHOD_CLS_HDR_REGEX captures both method and field headers in a class
    METHOD_CLS_HDR_REGEX = re.compile(
        r"#\d+\s+:\s+\(in L(?P<class>[A-Za-z][0-9A-Za-z]*\/[0-9A-Za-z_$\/]+);\)"
    )
    # METHOD_REGEX captures both method and field names in a class
    METHOD_REGEX = re.compile(r"name\s+:\s+\'(?P<method>[<A-Za-z][>A-Za-z0-9_$]*)\'")

    CLS_CHUNK_HDR_REGEX = re.compile(r"  [A-Z]")
    CLS_HDR_REGEX = re.compile(r"Class #")

    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps
        self.reading_methods = False
        self.current_class = None
        self.current_class_name = None
        self.current_method = None
        self.last_lineno = None
        self.prev_line = None

    def class_replacer(self, matchobj):
        m = matchobj.group("class")
        cls = m.replace("/", ".")
        if cls in self.symbol_maps.class_map:
            return "L%s;" % self.symbol_maps.class_map[cls].origin_class.replace(
                ".", "/"
            )
        return "L%s;" % m

    def line_replacer(self, matchobj):
        lineno = int(matchobj.group("lineno"))
        positions = map(
            lambda p: "%s:%d" % (p.file, p.line),
            self.symbol_maps.line_map.get_stack(lineno - 1),
        )
        return matchobj.group("prefix") + ", ".join(positions)

    def method_replacer(self, matchobj):
        m = matchobj.group(0)
        if self.current_class is not None:
            cls = self.current_class.replace("/", ".")
            if cls in self.symbol_maps.class_map:
                left, _sep, right = m.partition(": ")
                if self.reading_methods:
                    method_name = matchobj.group("method")
                    if method_name in self.symbol_maps.class_map[cls].method_mapping:
                        return (
                            left
                            + _sep
                            + self.symbol_maps.class_map[cls].method_mapping[
                                method_name
                            ]
                        )
                else:
                    field_name = matchobj.group("method")
                    if field_name in self.symbol_maps.class_map[cls].field_mapping:
                        return (
                            left
                            + _sep
                            + self.symbol_maps.class_map[cls].field_mapping[field_name]
                        )
        return m

    def reset_state(self):
        self.current_class = None
        self.current_class_name = None
        self.current_method = None
        self.last_lineno = None
        self.prev_line = None

    def symbolicate(self, line):
        symbolicated_line = self._symbolicate(line)
        self.prev_line = line
        return symbolicated_line

    def _symbolicate(self, line):
        if self.symbol_maps.iodi_metadata:
            match = self.METHOD_CLS_HDR_REGEX.search(line)
            if match:
                self.current_class = match.group("class")
            elif self.current_class:
                match = self.METHOD_REGEX.search(line)
                if match:
                    self.current_method = match.group("method")
                    self.current_class_name = self.current_class.replace("/", ".")
                    line = self.METHOD_REGEX.sub(self.method_replacer, line)
                elif self.current_method:
                    match = self.LINE_REGEX.search(line)
                    if match:
                        lineno = match.group("lineno")
                        mapped_line, _ = self.symbol_maps.iodi_metadata.map_iodi(
                            self.symbol_maps.debug_line_map,
                            self.current_class_name,
                            self.current_method,
                            lineno,
                        )
                        if mapped_line:
                            if self.last_lineno:
                                if self.last_lineno == mapped_line:
                                    # Don't emit duplicate line entries
                                    return None
                            self.last_lineno = mapped_line
                            positions = map(
                                lambda p: "%s:%d" % (p.file, p.line),
                                self.symbol_maps.line_map.get_stack(mapped_line - 1),
                            )
                            return (
                                "        "
                                + match.group("prefix")
                                + ", ".join(positions)
                                + "\n"
                            )
                    no_debug_info = (
                        "positions     :" in self.prev_line
                        and "locals        :" in line
                    )
                    if no_debug_info:
                        mappings = self.symbol_maps.iodi_metadata.map_iodi_no_debug_to_mappings(
                            self.symbol_maps.debug_line_map,
                            self.current_class_name,
                            self.current_method,
                        )
                        if mappings is None:
                            return line
                        result = ""
                        for i, (pc, mapped_line) in enumerate(mappings):
                            positions = map(
                                lambda p: "%s:%d" % (p.file, p.line),
                                self.symbol_maps.line_map.get_stack(mapped_line - 1),
                            )
                            if i == 0:
                                pc = 0
                            result += (
                                "        "
                                + "{0:#0{1}x}".format(pc, 6)
                                + " line="
                                + ", ".join(positions)
                                + "\n"
                            )
                        return result + line

            if self.CLS_CHUNK_HDR_REGEX.match(line):
                # If we match a header but its the wrong header then ignore the
                # contents of this subsection until we hit a methods subsection
                self.reading_methods = (
                    "Direct methods" in line or "Virtual methods" in line
                )
                if not self.reading_methods:
                    self.reset_state()
            elif self.CLS_HDR_REGEX.match(line):
                self.reading_methods = False
                self.reset_state()

        line = self.CLASS_REGEX.sub(self.class_replacer, line)
        line = self.LINE_REGEX.sub(self.line_replacer, line)
        return line

    @staticmethod
    def is_likely_dexdump(line):
        return re.match(r"^Processing '.*\.dex'", line) or re.search(
            r"Class #\d+", line
        )
