#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

"""
Block Profile to Lines Mapping Tool

Combines block profile CSV data with source block-to-lines mapping JSONL data
to produce a file listing executed source lines.

Usage:
    python block_profile_to_lines.py \
        --profile /path/to/block_profiles.csv \
        --mapping /path/to/sb-to-lines-mapping.jsonl \
        --output /path/to/output.csv \
        --format csv|json
"""

from __future__ import annotations

import argparse
import json
import logging
import re
import sys
from collections import defaultdict
from contextlib import AbstractContextManager, nullcontext
from dataclasses import dataclass
from typing import TextIO

logger: logging.Logger = logging.getLogger(__name__)


@dataclass
class BlockInfo:
    """Information about a source block from the profile expression."""

    block_id: int
    hit_value: float
    appear_value: float

    @property
    def is_hit(self) -> bool:
        """A block is 'hit' when the hit value is > 0."""
        return self.hit_value > 0


class ProfileExpressionParser:
    """
    Parse S-expression profile strings from the block profile CSV.

    Format: (X:Y ...) where X is hit value, Y is appear percentage
    Nested parentheses represent child blocks.
    Edge markers: g (goto), b (branch), t (throw) separate siblings.

    Example: "(0.2:0.3 g (0.1:0.1))" has two blocks:
      - Block 0: hit=0.2, appear=0.3
      - Block 1: hit=0.1, appear=0.1
    """

    BLOCK_PATTERN: re.Pattern[str] = re.compile(r"\((\d+(?:\.\d+)?):(\d+(?:\.\d+)?)")

    def parse(self, expression: str) -> list[BlockInfo]:
        """
        Parse a profile expression and extract block information.

        Returns a list of BlockInfo objects in block ID order.
        """
        if not expression or expression.strip() == "":
            return []

        blocks = []
        block_id = 0

        for match in self.BLOCK_PATTERN.finditer(expression):
            hit_value = float(match.group(1))
            appear_value = float(match.group(2))
            blocks.append(BlockInfo(block_id, hit_value, appear_value))
            block_id += 1

        return blocks

    def get_hit_block_ids(self, expression: str) -> set[int]:
        """Get the set of block IDs that were hit (value > 0)."""
        blocks = self.parse(expression)
        return {block.block_id for block in blocks if block.is_hit}


class CSVParser:
    """
    Parse block profile CSV files.

    Format:
    - Line 1: "interaction,appear#" or "interaction,appear#,build#"
    - Line 2: interaction name, interaction value, [build#]
    - Line 3: "name,profiled_srcblks_exprs"
    - Lines 4+: method_name,profile_expression
    """

    def parse(self, filepath: str) -> dict[str, str]:
        """
        Parse a block profile CSV file.

        Returns a dict mapping method_name -> profile_expression.
        """
        profiles: dict[str, str] = {}

        with open(filepath, encoding="utf-8") as f:
            lines = f.readlines()

        if len(lines) < 3:
            raise ValueError(
                f"CSV file {filepath} has too few lines (expected at least 3)"
            )

        line_idx = 0

        header1 = lines[line_idx].strip().split(",")
        if header1[0] != "interaction":
            raise ValueError(
                f"Expected 'interaction' in first column, got '{header1[0]}'"
            )
        line_idx += 1

        line_idx += 1

        header3 = lines[line_idx].strip().split(",")
        if header3[0] != "name" or header3[1] != "profiled_srcblks_exprs":
            raise ValueError(
                f"Expected 'name,profiled_srcblks_exprs' header, got '{lines[line_idx].strip()}'"
            )
        line_idx += 1

        for line in lines[line_idx:]:
            line = line.strip()
            if not line:
                continue

            first_comma = line.find(",")
            if first_comma == -1:
                continue

            method_name = line[:first_comma]
            profile_expr = line[first_comma + 1 :]

            if profile_expr:
                profiles[method_name] = profile_expr

        return profiles


class JSONLParser:
    """
    Parse source block to lines mapping JSONL files.

    Format: One JSON object per line with:
    - name: Method name
    - default_file: Source filename
    - block_list: Array where index = source block ID, value = array of line numbers
    """

    def __init__(self, profiles: dict[str, str]) -> None:
        """
        Initialize with profiles to enable streaming match during parse.

        Args:
            profiles: Dict mapping method_name -> profile_expression
        """
        self.profiles = profiles
        self.expression_parser = ProfileExpressionParser()
        self.matched = 0
        self.unmatched_profile = 0
        self.unmatched_mapping = 0
        self.matched_methods: set[str] = set()

    def parse_and_extract(self, filepath: str) -> dict[str, set[int]]:
        """
        Parse a JSONL mapping file and extract hit lines in a single pass.

        Returns dict mapping filename -> set of hit line numbers.
        """
        file_lines: dict[str, set[int]] = defaultdict(set)

        with open(filepath, encoding="utf-8") as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue

                try:
                    obj = json.loads(line)
                except json.JSONDecodeError as e:
                    logger.warning("Failed to parse JSON on line %d: %s", line_num, e)
                    continue

                name = obj.get("name")
                if not name:
                    continue

                if name not in self.profiles:
                    self.unmatched_mapping += 1
                    continue

                self.matched += 1
                self.matched_methods.add(name)
                profile_expr = self.profiles[name]
                default_file = obj.get("default_file", "UnknownSource")
                block_list = obj.get("block_list", [])

                if default_file == "UnknownSource":
                    continue

                hit_block_ids = self.expression_parser.get_hit_block_ids(profile_expr)

                for block_id in hit_block_ids:
                    if block_id < len(block_list):
                        lines = block_list[block_id]
                        for line_no in lines:
                            file_lines[default_file].add(line_no)

        self.unmatched_profile = len(self.profiles) - self.matched

        return file_lines

    def log_stats(self) -> None:
        """Log matching statistics."""
        logger.debug("Matched methods: %d", self.matched)
        logger.debug("Methods in profile but not mapping: %d", self.unmatched_profile)
        logger.debug("Methods in mapping but not profile: %d", self.unmatched_mapping)

    def get_unmatched_profile_methods(self) -> list[str]:
        """Get sorted list of method names in profile but not in mapping."""
        return sorted(set(self.profiles.keys()) - self.matched_methods)


class OutputWriter:
    """Write output in various formats."""

    def write_csv(
        self, file_lines: dict[str, set[int]], output_path: str | None
    ) -> None:
        """Write output in CSV format: filename,executed_lines"""
        # pyre-ignore[9]: nullcontext yields the provided value
        cm: AbstractContextManager[TextIO, None] = (
            nullcontext(sys.stdout) if output_path is None else open(output_path, "w")
        )

        with cm as output:
            output.write("filename,executed_lines\n")
            for filename in sorted(file_lines.keys()):
                lines = sorted(file_lines[filename])
                lines_str = ",".join(str(line) for line in lines)
                escaped_filename = filename.replace('"', '""')
                output.write(f'"{escaped_filename}","{lines_str}"\n')

    def write_json(
        self, file_lines: dict[str, set[int]], output_path: str | None
    ) -> None:
        """Write output in JSONL format: one JSON object per line."""
        # pyre-ignore[9]: nullcontext yields the provided value
        cm: AbstractContextManager[TextIO, None] = (
            nullcontext(sys.stdout) if output_path is None else open(output_path, "w")
        )

        with cm as output:
            for filename in sorted(file_lines.keys()):
                lines = sorted(file_lines[filename])
                record = {"filename": filename, "lines": lines}
                output.write(json.dumps(record))
                output.write("\n")

    def write(
        self,
        file_lines: dict[str, set[int]],
        output_path: str | None,
        output_format: str,
    ) -> None:
        """Write output in the specified format."""
        if output_format == "csv":
            self.write_csv(file_lines, output_path)
        elif output_format == "json":
            self.write_json(file_lines, output_path)
        else:
            raise ValueError(f"Unknown output format: {output_format}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Combine block profile CSV with source block-to-lines mapping to produce executed lines"
    )
    parser.add_argument(
        "--profile",
        "-p",
        required=True,
        help="Path to block profile CSV file (block_profiles_*.csv)",
    )
    parser.add_argument(
        "--mapping",
        "-m",
        required=True,
        help="Path to source block to lines mapping JSONL file (*-isb-sb-to-lines-mapping.jsonl)",
    )
    parser.add_argument(
        "--output",
        "-o",
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "--format",
        "-f",
        choices=["csv", "json"],
        default="csv",
        help="Output format (default: csv)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Enable debug logging",
    )
    parser.add_argument(
        "--unmatched-output",
        "-u",
        help="Output file for method names in profile but not in mapping",
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.WARNING,
        format="[%(levelname)-8s][%(asctime)-23s][%(name)-16s] %(message)s",
    )

    csv_parser = CSVParser()
    profiles = csv_parser.parse(args.profile)
    logger.debug("Loaded %d method profiles from CSV", len(profiles))

    jsonl_parser = JSONLParser(profiles)
    file_lines = jsonl_parser.parse_and_extract(args.mapping)
    jsonl_parser.log_stats()

    total_lines = sum(len(lines) for lines in file_lines.values())
    logger.debug("Found %d hit lines across %d files", total_lines, len(file_lines))

    writer = OutputWriter()
    writer.write(file_lines, args.output, args.format)

    if args.unmatched_output:
        unmatched_methods = jsonl_parser.get_unmatched_profile_methods()
        with open(args.unmatched_output, "w") as f:
            for method in unmatched_methods:
                f.write(method)
                f.write("\n")
        logger.debug(
            "Wrote %d unmatched methods to %s",
            len(unmatched_methods),
            args.unmatched_output,
        )


if __name__ == "__main__":
    main()
