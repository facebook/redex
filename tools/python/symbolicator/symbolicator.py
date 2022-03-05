#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import argparse
import collections
import io
import logging
import os
import signal
import sys

from debug_line_map import DebugLineMap  # pyre-fixme[21]
from dexdump import DexdumpSymbolicator  # pyre-fixme[21]
from iodi import IODIMetadata  # pyre-fixme[21]
from line_unmap import PositionMap  # pyre-fixme[21]
from logcat import LogcatSymbolicator  # pyre-fixme[21]
from symbol_files import SymbolFiles  # pyre-fixme[21]


# A simple symbolicator for line-based input,
# i.e. a newline separated list of class names to be symbolicated.
# X.A01 = > com/facebook/XyzClass.class
class LinesSymbolicator(object):
    def __init__(self, class_map, skip_unsymbolicated):
        # We only need class map for this symbolicator.
        self.class_map = class_map
        self.skip_unsymbolicated = skip_unsymbolicated

    def symbolicate(self, line):
        if line.endswith(".class"):
            class_name = line[:-7]  # strip '.class' suffix.
        else:
            class_name = line[:-1]
        class_name = class_name.replace("/", ".")

        if class_name in self.class_map:
            ans = self.class_map[class_name].origin_class
            return ans.replace(".", "/") + ".class\n"
        return "" if self.skip_unsymbolicated else line


class SymbolMaps(object):
    def __init__(self, symbol_files):
        self.class_map = self.get_class_map(symbol_files.extracted_symbols)
        self.line_map = PositionMap.read_from(symbol_files.line_map)
        self.debug_line_map = (
            DebugLineMap.read_from(symbol_files.debug_line_map)
            if os.path.exists(symbol_files.debug_line_map)
            else None
        )
        if os.path.exists(symbol_files.iodi_metadata):
            if not self.debug_line_map:
                logging.error(
                    "In order to symbolicate with IODI, redex-debug-line-map-v2 is required!"
                )
                sys.exit(1)
            self.iodi_metadata = IODIMetadata(symbol_files.iodi_metadata)
            logging.info(
                "Unpacked "
                + str(len(self.iodi_metadata._entries))
                + " methods from iodi metadata"
            )
        else:
            self.iodi_metadata = None

    @staticmethod
    def get_class_map(mapping_filename):
        mapping = {}
        current_class = None
        with open(mapping_filename) as f:
            for line in f:
                if " -> " in line:
                    original, _sep, new = line.partition(" -> ")
                    if line[0] != " ":
                        new = new.strip()[:-1]
                        ClassMemberMapping = collections.namedtuple(
                            "ClassMemberMapping",
                            ["origin_class", "method_mapping", "field_mapping"],
                        )
                        mapping[new] = ClassMemberMapping(original, {}, {})
                        current_class = new
                    else:
                        if current_class is None:
                            logging.error(
                                "The member "
                                + original
                                + " , obfuscated as "
                                + new
                                + " does not belong to any class!"
                            )
                        original = original.strip()
                        if "(" in original and original[len(original) - 1] == ")":
                            mapping[current_class].method_mapping[
                                new.strip()
                            ] = original.split()[-1].split("(")[0]
                        else:
                            mapping[current_class].field_mapping[
                                new.strip()
                            ] = original.split()[-1]
        return mapping


def parse_args(args):
    """
    args is a list of (str, dict) tuples that should be passed to
    ArgumentParser.add_argument with the initial str as the flag
    """
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="""=== Android Symbolicator ===

Supports logcat and dexdump inputs when piped to stdin.

Examples of usage:

    adb logcat | ./symbolicator.py --artifacts /path/to/artifacts/

    dexdump -d secondary-1.dex | ./symbolicator.py --artifacts /artifacts/
""",
    )

    parser.add_argument(
        "--target",
        type=str,
        help="Buck target that's been built. This will be slow as it "
        "queries buck. Additionally it requires the cwd to be a subdir "
        "of a buck directory. Pass the artifact directory directly if "
        "you know it",
    )
    parser.add_argument(
        "--artifacts",
        type=str,
        help="e.g. ~/buckrepo/buck-out/gen/path/to/app/artifacts/",
    )
    parser.add_argument(
        "--input-type", type=str, choices=("logcat", "dexdump", "lines")
    )
    parser.add_argument("--skip-unsymbolicated", action="store_true")

    parser.add_argument(
        "--log-level",
        default="info",
        help="Specify the python logging level",
    )

    if args is not None:
        for flag, arg in args:
            parser.add_argument(flag, **arg)

    return parser.parse_args()


def _init_logging(level_str: str) -> None:
    levels = {
        "critical": logging.CRITICAL,
        "error": logging.ERROR,
        "warn": logging.WARNING,
        "warning": logging.WARNING,
        "info": logging.INFO,
        "debug": logging.DEBUG,
    }
    level = levels[level_str]
    logging.basicConfig(level=level)


def main(arg_desc=None, symbol_file_generator=None):
    args = parse_args(arg_desc)

    _init_logging(args.log_level)

    if args.skip_unsymbolicated and args.input_type != "lines":
        logging.warning(
            "'--skip-unsymbolicated' is not needed, it only works with '--input-type lines'"
        )

    if args.artifacts is not None:
        symbol_files = SymbolFiles.from_buck_artifact_dir(args.artifacts)
    elif args.target is not None:
        symbol_files = SymbolFiles.from_buck_target(args.target)
    elif symbol_file_generator is not None:
        symbol_files = symbol_file_generator(args)
    if symbol_files is None:
        logging.error(
            "Unable to find symbol files used to symbolicate input!"
            "Try passing --target or --artifacts."
        )
        sys.exit(1)

    # I occasionally use ctrl-C to e.g. terminate a text search in `less`; I
    # don't want to kill this script in the process, though
    if not sys.stdout.isatty():
        signal.signal(signal.SIGINT, signal.SIG_IGN)

    reader = io.TextIOWrapper(
        sys.stdin.buffer, encoding="utf-8", errors="surrogateescape"
    )
    writer = io.TextIOWrapper(
        sys.stdout.buffer, encoding="utf-8", errors="surrogateescape"
    )

    first_line = ""
    while first_line == "":
        try:
            first_line = next(reader)
        except StopIteration:
            logging.warning("Empty input")
            sys.exit(0)

    if args.input_type == "lines":
        class_map = SymbolMaps.get_class_map(symbol_files.extracted_symbols)
        symbolicator = LinesSymbolicator(class_map, args.skip_unsymbolicated)
    elif args.input_type == "logcat" or LogcatSymbolicator.is_likely_logcat(first_line):
        symbol_maps = SymbolMaps(symbol_files)
        symbolicator = LogcatSymbolicator(symbol_maps)
    elif args.input_type == "dexdump" or DexdumpSymbolicator.is_likely_dexdump(
        first_line
    ):
        symbol_maps = SymbolMaps(symbol_files)
        symbolicator = DexdumpSymbolicator(symbol_maps)
    else:
        symbol_maps = SymbolMaps(symbol_files)
        logging.warning("Could not figure out input kind, assuming logcat")
        symbolicator = LogcatSymbolicator(symbol_maps)

    logging.info("Using %s", type(symbolicator).__name__)

    def output(s):
        if s is not None:
            writer.write(s)

    output(symbolicator.symbolicate(first_line))
    for line in reader:
        output(symbolicator.symbolicate(line))


if __name__ == "__main__":
    main()
