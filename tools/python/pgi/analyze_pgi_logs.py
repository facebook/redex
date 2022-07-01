#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Process logging from Redex when using PGI, outputting features of
# the caller-callee pairs.
#
# To use, run Redex with `TRACE=PM:1,METH_PROF:5,MMINL:4` and feed
# the log to this script.

import csv
import logging
import re
import sys
from collections import namedtuple


# Connect a line of a file with its line number. For better error legibility.
Line = namedtuple("Line", ["data", "no"])


# Generate a stream of `Line` elements from a file.
def _gen_lines(filename):
    with open(filename) as f:
        no = 0
        for line in f:
            yield Line(line.strip(), no)
            no += 1


# Collects the data given by the inliner. These are the method stats at the
# time of inlining and may be different from at the time of the inlining
# decision. For the callee, `callee_depth` will be zero.
RawMethodDataAtInline = namedtuple(
    "RawMethodDataAtInline",
    [
        "method_name",
        "regs",
        "insns",
        "blocks",
        "edges",
        "num_loops",
        "deepest_loop",
        "callee_depth",
    ],
)


# Parse the (stripped) line containing inliner data per method.
def _parse_raw_method_data_at_inline(method_name, arr):
    return RawMethodDataAtInline(
        method_name,
        int(arr[0]),
        int(arr[1]),
        int(arr[2]),
        int(arr[3]),
        int(arr[4]),
        int(arr[5]),
        int(arr[6]),
    )


# A pair of `RawMethodDataAtInline` data elements.
RawInlinePair = namedtuple("RawInlinePair", ["caller", "callee"])


# Consume all `Line`s from the given stream, extracting `RawInlinePair`s. Any
# non-`Line` elements are yielded. All `Line` elements are consumed.
def _gen_raw_inline_pairs(iterator):
    regex = re.compile(r"^inline ([^ ]+) into ([^ ]+) (.*)")
    for line in iterator:
        if not isinstance(line, Line):
            yield line
            continue

        match = regex.match(line.data)
        if not match:
            yield line
            continue

        caller_name = match.group(2)
        callee_name = match.group(1)

        try:
            data_parts = match.group(3).split("!")
            if len(data_parts) != 2 * 7:
                raise ValueError(f"Cannot parse {match.group(3)}")

            caller_data = _parse_raw_method_data_at_inline(caller_name, data_parts[0:7])
            callee_data = _parse_raw_method_data_at_inline(
                callee_name, data_parts[7:14]
            )
            yield RawInlinePair(caller=caller_data, callee=callee_data)
        except BaseException as e:
            logging.error("%s(%s)", e, type(e))
            raise ValueError(f"{line}: {e} - {iterator}/{type(iterator)}")


# The caller/callee statistics of a PGI decision.
def _identity(input):
    return input


SingleMetrics = [
    ("params", int),
    ("blocks", int),
    ("edges", int),
    ("hits", float),
    ("appear100", float),
    ("insns", int),
    ("opcodes", int),
    ("regs", int),
    ("num_loops", int),
    ("deepest_loop", int),
]
InlineForSpeedDataTyping = [
    ("caller", _identity),
    *[("caller_" + name, fn) for name, fn in SingleMetrics],
    ("callee", _identity),
    *[("callee_" + name, fn) for name, fn in SingleMetrics],
    ("interaction", _identity),
    ("confidence", float),
]
InlineForSpeedData = namedtuple(
    "InlineForSpeedData",
    [name for name, _type in InlineForSpeedDataTyping],
)


# Parse PGI inlining decisions from the given stream. Other elements are
# yielded.
def _gen_profile_decisions(iterator):
    header = "[InlineForSpeedDecisionTrees]"

    for line in iterator:
        if not isinstance(line, Line):
            yield line
            continue

        if not line.data.startswith(header):
            yield line
            continue

        data = line.data[line.data.index(":") + 1 :]
        data_parts = data.split("!")
        conf_str = line.data[len(header) : line.data.index(":")].strip()
        data_parts.append(conf_str)

        if len(data_parts) != len(InlineForSpeedDataTyping):
            raise ValueError(f"{line}: {data_parts}")

        try:
            yield InlineForSpeedData._make(
                (
                    fn(data_parts[i].strip())
                    for i, (_name, fn) in enumerate(InlineForSpeedDataTyping)
                )
            )
        except BaseException as e:
            raise ValueError(f"{line}: {e}")


# Direct lines that belong to a pass to a handler.
def _gen_pass(iterator, pass_name, handler):
    found = False
    start_string = f"Running {pass_name}..."
    end_str_prefix = "After processing unresolved lines:"
    for line in iterator:
        if isinstance(line, Line):
            if line.data == start_string:
                assert not found
                found = True
                print(f"Found {pass_name} at line {line.no}")
                handler.start()
                continue
            elif found and line.data.startswith(end_str_prefix):
                found = False
                res = handler.finish()
                print(f"Finished {pass_name} at line {line.no}")
                if res is not None:
                    yield res
                continue

        if found:
            res = handler.next_line(line)
            if res is not None:
                yield res
        else:
            yield line


# Named statistics of callees. Used to collect the stats for the caller/callee
# pairs.
InlineCalleeStats = namedtuple(
    "InlineCalleeStats",
    [
        "count",
        "max_loop_depth",
    ],
)


# Take a `RawInlinePair` and fill the given maps. Arbitrarily takes the first
# appearance of caller and callee for the respective maps. The `inline_data_map`
# is filled with the stats of callee per caller.
def _fill_inline_maps(data, caller_map, callee_map, inline_data_map):
    if not isinstance(data, RawInlinePair):
        return data

    if data.caller.method_name not in caller_map:
        caller_map[data.caller.method_name] = data.caller

    if data.callee.method_name not in callee_map:
        callee_map[data.callee.method_name] = data.callee

    stats = inline_data_map.setdefault(data.caller.method_name, {}).setdefault(
        data.callee.method_name, InlineCalleeStats(0, 0)
    )
    inline_data_map[data.caller.method_name][
        data.callee.method_name
    ] = InlineCalleeStats(
        stats.count + 1, max(stats.max_loop_depth, data.caller.callee_depth)
    )

    return None


ColumnOrder = [
    "caller",
    "callee",
    *["caller_" + name for name, _type in SingleMetrics],
    *["callee_" + name for name, _type in SingleMetrics],
    "inline_count",
    "inline_max_loop_depth",
    "interaction",
    "confidence",
]


for name, _type in InlineForSpeedDataTyping:
    assert name in ColumnOrder


# Common column description for CSV files.
def _write_csv_header(csv_writer):
    csv_writer.writerow(ColumnOrder)


def _make_row(
    caller,
    caller_data,
    callee,
    callee_data,
    inline_data,
    interaction,
    confidence,
    ifs=None,
):
    tmp = {
        "caller": caller,
        "callee": callee,
        "inline_count": inline_data.count,
        "inline_max_loop_depth": inline_data.max_loop_depth,
        "interaction": interaction,
        "confidence": confidence,
    }

    for name, _type in SingleMetrics:

        def get_val(name, full_name, data):
            if hasattr(data, name):
                return getattr(data, name)
            if ifs is None:
                return -1
            return getattr(ifs, full_name)

        tmp["caller_" + name] = get_val(name, "caller_" + name, caller_data)
        tmp["callee_" + name] = get_val(name, "callee_" + name, callee_data)

    row = [tmp[name] for name in ColumnOrder]

    return row


class MaybePrintDot:
    def __init__(self, threshold, char="."):
        self._threshold = threshold
        self._count = 0
        self._printed = False
        self._char = char

    def reset(self):
        self._count = 0
        self._printed = False

    def reset_newline(self):
        if self._printed:
            print("")
        self.reset()

    def maybe_print(self, val):
        if val is None:
            return val

        self._count += 1
        if self._count % self._threshold == 0:
            print(self._char, end="")
            self._printed = True
        return val

    def maybe_print_rev(self, val):
        if val is None:
            self.maybe_print(1)
        return val

    def printed(self):
        return self._printed


# Parse the standard MethodInlinePass.
class MethodInlinerPassHandler:
    def __init__(self, out_suffix):
        self._run = 0
        self._out_suffix = out_suffix
        self._printer = MaybePrintDot(1000)
        pass

    def start(self):
        self._caller_map = {}
        self._callee_map = {}
        self._inline_data_map = {}
        self._run += 1
        self._printer.reset()
        pass

    def next_line(self, line):
        return self._printer.maybe_print_rev(
            _fill_inline_maps(
                line, self._caller_map, self._callee_map, self._inline_data_map
            )
        )

    def finish(self):
        lines = 0
        self._printer.reset_newline()

        printer = MaybePrintDot(1000, "!")
        with open(
            f"non-pgi-{self._out_suffix}-{self._run}.csv", "w", newline=""
        ) as out_file:
            csv_writer = csv.writer(out_file, delimiter=",")
            _write_csv_header(csv_writer)

            for caller, caller_callee_map in sorted(
                self._inline_data_map.items(), key=lambda x: x[0]
            ):
                for callee, inline_data in sorted(
                    caller_callee_map.items(), key=lambda x: x[0]
                ):
                    row = _make_row(
                        caller=caller,
                        caller_data=self._caller_map[caller],
                        callee=callee,
                        callee_data=self._callee_map[callee],
                        inline_data=inline_data,
                        interaction="n/a",
                        confidence=-1,
                        ifs=None,
                    )
                    csv_writer.writerow(row)

                    lines += 1
                    printer.maybe_print(1)

        printer.reset_newline()
        print(f"Wrote {lines} lines for {self._out_suffix} run {self._run}")
        return None


class PGIHandler:
    def __init__(self, aggregate_ifs):
        self._run = 0
        self._printer = MaybePrintDot(1000)
        self._aggregate_ifs = aggregate_ifs
        pass

    def start(self):
        self._inline_for_speed_list = []
        self._caller_map = {}
        self._callee_map = {}
        self._inline_data_map = {}
        self._run += 1
        self._printer.reset()
        pass

    def next_line(self, data):
        if isinstance(data, InlineForSpeedData):
            self._inline_for_speed_list.append(data)
            return self._printer.maybe_print_rev(None)

        return self._printer.maybe_print_rev(
            _fill_inline_maps(
                data, self._caller_map, self._callee_map, self._inline_data_map
            )
        )

    def finish(self):
        self._printer.reset_newline()

        ifs_inlined = [
            ifs
            for ifs in self._inline_for_speed_list
            if ifs.callee in self._inline_data_map.get(ifs.caller, {})
        ]
        print(
            f"PGI run {self._run}: IFS lines={len(self._inline_for_speed_list)} Inlines={len(ifs_inlined)}"
        )

        def ifs_key(ifs):
            return ifs.caller + "#" + ifs.callee

        if self._aggregate_ifs:
            ifs_map = {}
            for ifs in ifs_inlined:
                ifs_map.setdefault(ifs_key(ifs), set()).add(ifs)

            def combine_max(ifs_set):
                max_pair = (
                    max(ifs.caller_hits for ifs in ifs_set),
                    max(ifs.callee_hits for ifs in ifs_set),
                )
                ifs = next(iter(ifs_set))
                return ifs._replace(
                    caller_hits=max_pair[0],
                    callee_hits=max_pair[1],
                )

            old_size = len(ifs_inlined)
            ifs_inlined = [combine_max(ifs_set) for ifs_set in ifs_map.values()]
            new_size = len(ifs_inlined)
            print(f"Aggregated IFS inlines: {old_size} -> {new_size}")

        ifs_inlined.sort(key=ifs_key)

        printer = MaybePrintDot(1000, "!")
        with open(f"pgi-{self._run}.csv", "w", newline="") as out_file:
            csv_writer = csv.writer(out_file, delimiter=",")
            _write_csv_header(csv_writer)

            for ifs in ifs_inlined:
                row = _make_row(
                    caller=ifs.caller,
                    caller_data=self._caller_map[ifs.caller],
                    callee=ifs.callee,
                    callee_data=self._callee_map[ifs.callee],
                    inline_data=self._inline_data_map[ifs.caller][ifs.callee],
                    interaction="aggregate" if self._aggregate_ifs else ifs.interaction,
                    confidence=ifs.confidence,
                    ifs=ifs,
                )
                csv_writer.writerow(row)
                printer.maybe_print(1)

        printer.reset_newline()
        return None


if __name__ == "__main__":
    filename = sys.argv[1]

    aggregate_ifs = len(sys.argv) > 2 and sys.argv[2] == "--aggregate-ifs"

    gen = _gen_lines(filename)
    gen = _gen_raw_inline_pairs(gen)
    records_parsed = _gen_profile_decisions(gen)

    gen = _gen_pass(
        records_parsed, "MethodInlinePass", MethodInlinerPassHandler("method-inline")
    )
    gen = _gen_pass(
        gen, "IntraDexInlinePass", MethodInlinerPassHandler("intradex-inline")
    )
    passes = _gen_pass(gen, "PerfMethodInlinePass", PGIHandler(aggregate_ifs))

    # Drain the stream.
    for _ in passes:
        pass
