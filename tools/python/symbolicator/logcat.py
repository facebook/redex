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
        r"\.(?P<method>[0-9A-Za-z_$<>]+)\(((Unknown Source)?:(?P<lineno>\d+))?\)\s*\n",
        re.MULTILINE,
    )

    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps
        self.pending_switches = []

    def class_replacer(self, matchobj):
        m = matchobj.group(0)
        if m in self.symbol_maps.class_map:
            return self.symbol_maps.class_map[m].origin_class
        return m

    def find_case_positions(self, start, pattern_id):
        count_positions = self.symbol_maps.line_map.get_stack(start)
        assert len(count_positions) == 1
        assert count_positions[0].method == "redex.$Position.count"
        # The cases are stored in the immediately following lines,
        # and are ordered by pattern-id, so we can do a binary search.
        end = start + count_positions[0].line
        start = start + 1
        while start <= end:
            middle = (start + end) // 2
            case_positions = self.symbol_maps.line_map.get_stack(middle)
            assert len(case_positions) >= 1
            assert case_positions[0].method == "redex.$Position.case"
            if case_positions[0].line == pattern_id:
                case_positions.pop(0)
                return case_positions
            elif case_positions[0].line < pattern_id:
                start = middle + 1
            else:
                end = middle - 1
        return None

    # If there's no debug info item, stack traces have no line number e.g.
    #   at X.OPu.A04()
    # Just deobfuscate the class/method name
    def line_replacer_no_lineno(self, matchobj):
        class_name = matchobj.group("class")
        method_name = matchobj.group("method")
        if class_name in self.symbol_maps.class_map:
            class_map = self.symbol_maps.class_map[class_name]
            deobf_class_name = class_map.origin_class
            deobf_method_name = class_map.method_mapping[method_name]
            return "%s\tat %s.%s()\n" % (
                matchobj.group("prefix"),
                deobf_class_name,
                deobf_method_name,
            )
        return matchobj.string

    def line_replacer(self, matchobj):
        if not matchobj.group("lineno"):
            return self.line_replacer_no_lineno(matchobj)

        lineno = int(matchobj.group("lineno"))
        cls = matchobj.group("class")
        if self.symbol_maps.iodi_metadata is not None:
            mapped_lineno, _ = self.symbol_maps.iodi_metadata.map_iodi(
                self.symbol_maps.debug_line_map,
                cls,
                matchobj.group("method"),
                lineno,
            )
            lineno = mapped_lineno if mapped_lineno else lineno
        positions = self.symbol_maps.line_map.get_stack(lineno - 1)
        if cls in self.symbol_maps.class_map:
            cls = self.symbol_maps.class_map[cls]
        result = ""
        while positions:
            pos = positions.pop(0)
            if pos.method == "redex.$Position.switch":
                self.pending_switches.append(
                    {"prefix": matchobj.group("prefix"), "line": pos.line}
                )
            elif pos.method == "redex.$Position.pattern":
                pattern_id = pos.line
                if self.pending_switches:
                    ps = self.pending_switches.pop()
                    case_positions = self.find_case_positions(ps["line"], pattern_id)
                    if case_positions:
                        case_positions.extend(positions)
                        positions = case_positions
                        continue
                result += "%s\t$(unresolved switch %d)\n" % (
                    matchobj.group("prefix"),
                    pattern_id,
                )
            elif pos.method is None:
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
