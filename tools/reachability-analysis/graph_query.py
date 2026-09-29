#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import csv
import json
import sqlite3
import sys
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from typing import TextIO

from lib.sqlite_query import (
    KINDS,
    neighbors as query_neighbors,
    open_database,
    resolve_node,
    roots as query_roots,
    semantic_roots as query_semantic_roots,
    shortest_path,
)


def _nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return parsed


def _roots(connection: sqlite3.Connection, args) -> Iterable[Mapping[str, object]]:
    return query_roots(connection, args.kind, args.limit)


def _semantic_roots(
    connection: sqlite3.Connection,
) -> Iterable[Mapping[str, object]]:
    return query_semantic_roots(connection)


def _neighbors(
    connection: sqlite3.Connection,
    args,
) -> Iterable[Mapping[str, object]]:
    node = resolve_node(connection, args.name, args.kind)
    return query_neighbors(connection, node, args.direction)


def _path(
    connection: sqlite3.Connection,
    args,
) -> Iterable[Mapping[str, object]]:
    start = resolve_node(
        connection,
        args.from_name,
        args.from_kind,
        kind_option="--from-kind",
    )
    end = resolve_node(
        connection,
        args.to_name,
        args.to_kind,
        kind_option="--to-kind",
    )
    path = shortest_path(connection, start, end)
    if path is None:
        return [{"status": "no path", "step": "", "type": "", "name": ""}]
    return path


def _write_table(
    rows: Iterable[Mapping[str, object]],
    columns: Sequence[str],
    output: TextIO,
) -> None:
    widths = [len(column) for column in columns]
    with tempfile.TemporaryFile(mode="w+", newline="") as spool:
        writer = csv.writer(spool)
        for row in rows:
            values = [str(row[column]) for column in columns]
            writer.writerow(values)
            widths = [max(width, len(value)) for width, value in zip(widths, values)]

        output.write(
            "  ".join(column.ljust(width) for column, width in zip(columns, widths))
        )
        output.write("\n")
        output.write("  ".join("-" * width for width in widths))
        output.write("\n")
        spool.seek(0)
        for values in csv.reader(spool):
            output.write(
                "  ".join(value.ljust(width) for value, width in zip(values, widths))
            )
            output.write("\n")


def _write_json(
    rows: Iterable[Mapping[str, object]],
    output: TextIO,
) -> None:
    first = True
    output.write("[")
    for row in rows:
        output.write("\n" if first else ",\n")
        rendered = json.dumps(dict(row), indent=2)
        output.write("  " + rendered.replace("\n", "\n  "))
        first = False
    output.write("]\n" if first else "\n]\n")


def _write_rows(
    rows: Iterable[Mapping[str, object]],
    columns: Sequence[str],
    output_format: str,
    output: TextIO,
) -> None:
    if output_format == "json":
        _write_json(rows, output)
        return
    if output_format == "csv":
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
        return
    _write_table(rows, columns, output)


def _add_subcommands(parser: argparse.ArgumentParser) -> None:
    subparsers = parser.add_subparsers(dest="command", required=True)

    roots = subparsers.add_parser("roots", help="List zero-predecessor nodes")
    roots.add_argument("--kind", type=str.upper, choices=KINDS)
    roots.add_argument("--limit", type=_nonnegative_int)

    subparsers.add_parser(
        "semantic-roots",
        help="List program roots with their seed reasons",
    )

    neighbors = subparsers.add_parser("neighbors", help="List a node's neighbors")
    neighbors.add_argument("name")
    neighbors.add_argument("--kind", type=str.upper, choices=KINDS)
    neighbors.add_argument(
        "--direction",
        choices=("retainers", "retained", "both"),
        default="both",
    )

    path = subparsers.add_parser(
        "path",
        help="Find a shortest retainer-to-retained path",
        description=(
            "Find an unweighted shortest path following reachability edges from "
            "retainer to retained node."
        ),
    )
    path.add_argument("from_name", metavar="from")
    path.add_argument("to_name", metavar="to")
    path.add_argument("--from-kind", type=str.upper, choices=KINDS)
    path.add_argument("--to-kind", type=str.upper, choices=KINDS)


def parse_args(argv: Sequence[str]):
    parser = argparse.ArgumentParser(
        description="Run non-interactive queries on a Redex reachability database."
    )
    parser.add_argument(
        "--db",
        required=True,
        help="SQLite database generated by graph_to_sqlite",
    )
    parser.add_argument(
        "--format",
        choices=("table", "csv", "json"),
        default="table",
        help="Output format (default: table)",
    )
    _add_subcommands(parser)
    return parser.parse_args(argv)


def _run_query(connection: sqlite3.Connection, args):
    if args.command == "roots":
        return ("type", "name"), _roots(connection, args)
    if args.command == "semantic-roots":
        return ("type", "name", "reason"), _semantic_roots(connection)
    if args.command == "neighbors":
        return ("direction", "type", "name"), _neighbors(connection, args)
    if args.command == "path":
        return ("status", "step", "type", "name"), _path(connection, args)
    raise ValueError(f"Unknown command {args.command!r}")


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    try:
        with open_database(args.db) as connection:
            columns, rows = _run_query(connection, args)
            _write_rows(rows, columns, args.format, sys.stdout)
    except (OSError, sqlite3.Error, ValueError) as error:
        sys.stderr.write(f"Error: {error}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
