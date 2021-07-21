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
InlineForSpeedData = namedtuple(
    "InlineForSpeedData",
    [
        "caller",
        "caller_blocks",
        "caller_edges",
        "caller_hits",
        "caller_insns",
        "caller_regs",
        "caller_num_loops",
        "caller_deepest_loop",
        "callee",
        "callee_blocks",
        "callee_edges",
        "callee_hits",
        "callee_insns",
        "callee_regs",
        "callee_num_loops",
        "callee_deepest_loop",
        "interaction",
        "confidence",
    ],
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
        if len(data_parts) != 2 * 8 + 1:
            raise ValueError(f"{line}: {data_parts}")

        conf_str = line.data[len(header) : line.data.index(":")].strip()

        try:
            yield InlineForSpeedData(
                data_parts[0].strip(),
                int(data_parts[1].strip()),
                int(data_parts[2].strip()),
                float(data_parts[3].strip()),
                int(data_parts[4].strip()),
                int(data_parts[5].strip()),
                int(data_parts[6].strip()),
                int(data_parts[7].strip()),
                data_parts[8].strip(),
                int(data_parts[9].strip()),
                int(data_parts[10].strip()),
                float(data_parts[11].strip()),
                int(data_parts[12].strip()),
                int(data_parts[13].strip()),
                int(data_parts[14].strip()),
                int(data_parts[15].strip()),
                data_parts[16].strip(),
                int(conf_str),
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


# Common column description for CSV files.
def _write_csv_header(csv_writer, extra=None):
    header = [
        "caller",
        "callee",
        "caller_insns",
        "caller_regs",
        "caller_blocks",
        "caller_edges",
        "caller_num_loops",
        "caller_deepest_loop",
        "callee_insns",
        "callee_regs",
        "callee_blocks",
        "callee_edges",
        "callee_num_loops",
        "callee_deepest_loop",
        "caller_hits",
        "callee_hits",
        "inline_count",
        "inline_max_loop_depth",
    ]
    if extra is not None:
        header = header + list(extra)
    csv_writer.writerow(header)


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
                    caller_data = self._caller_map[caller]
                    callee_data = self._callee_map[callee]
                    row = (
                        caller,
                        callee,
                        caller_data.insns,
                        caller_data.regs,
                        caller_data.blocks,
                        caller_data.edges,
                        caller_data.num_loops,
                        caller_data.deepest_loop,
                        callee_data.insns,
                        callee_data.regs,
                        callee_data.blocks,
                        callee_data.edges,
                        callee_data.num_loops,
                        callee_data.deepest_loop,
                        0,
                        0,
                        inline_data.count,
                        inline_data.max_loop_depth,
                        0,
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
            header_extra = ([] if self._aggregate_ifs else ["interaction"]) + [
                "confidence"
            ]
            _write_csv_header(csv_writer, extra=header_extra)

            for ifs in ifs_inlined:
                caller_data = self._caller_map[ifs.caller]
                callee_data = self._callee_map[ifs.callee]
                inline_data = self._inline_data_map[ifs.caller][ifs.callee]
                row = (
                    [
                        ifs.caller,
                        ifs.callee,
                        caller_data.insns,
                        caller_data.regs,
                        caller_data.blocks,
                        caller_data.edges,
                        caller_data.num_loops,
                        caller_data.deepest_loop,
                        callee_data.insns,
                        callee_data.regs,
                        callee_data.blocks,
                        callee_data.edges,
                        callee_data.num_loops,
                        callee_data.deepest_loop,
                        ifs.caller_hits,
                        ifs.callee_hits,
                        inline_data.count,
                        inline_data.max_loop_depth,
                    ]
                    + ([] if self._aggregate_ifs else [ifs.interaction])
                    + ([ifs.confidence])
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
