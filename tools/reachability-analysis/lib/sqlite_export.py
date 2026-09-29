#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import sqlite3

from .core import ReachabilityGraph, ReachableObjectType


SCHEMA_VERSION = 1
_BATCH_SIZE = 10000
EDGE_DIRECTION = (
    "Each edges row means retainer_id retains retained_id. "
    "The binary graph stores each retained target followed by its predecessor "
    "retainer IDs."
)

_SCHEMA_SQL = """
CREATE TABLE nodes(
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('ANNO', 'CLASS', 'FIELD', 'METHOD', 'SEED')),
  name TEXT NOT NULL
);
CREATE TABLE edges(
  retainer_id INTEGER NOT NULL,
  retained_id INTEGER NOT NULL,
  FOREIGN KEY(retainer_id) REFERENCES nodes(id),
  FOREIGN KEY(retained_id) REFERENCES nodes(id)
);
CREATE TABLE meta(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
"""


def _remove_partial_output(output_filename):
    try:
        os.unlink(output_filename)
    except FileNotFoundError:
        pass


def _flush_batches(connection, node_rows, edge_rows):
    if node_rows:
        connection.executemany(
            "INSERT INTO nodes(id, kind, name) VALUES (?, ?, ?)", node_rows
        )
        node_rows.clear()
    if edge_rows:
        connection.executemany(
            "INSERT INTO edges(retainer_id, retained_id) VALUES (?, ?)", edge_rows
        )
        edge_rows.clear()


def export_graph(input_filename, output_filename):
    if os.path.exists(output_filename):
        raise FileExistsError("Output file already exists: %s" % output_filename)

    connection = None
    try:
        connection = sqlite3.connect(output_filename)
        connection.execute("PRAGMA foreign_keys = ON")
        connection.executescript(_SCHEMA_SQL)
        graph = ReachabilityGraph()
        with connection:
            connection.executemany(
                "INSERT INTO meta(key, value) VALUES (?, ?)",
                (
                    ("schema_version", str(SCHEMA_VERSION)),
                    ("source_graph", os.path.abspath(input_filename)),
                    ("source_format", "Redex reachability graph binary v1"),
                    ("edge_direction", EDGE_DIRECTION),
                ),
            )
            node_rows = []
            for node_id, node, _predecessor_ids in graph.iter_serialized_records(
                input_filename
            ):
                node_rows.append(
                    (
                        node_id,
                        ReachableObjectType.to_string(node.type),
                        node.name,
                    )
                )
                if len(node_rows) >= _BATCH_SIZE:
                    _flush_batches(connection, node_rows, [])
            _flush_batches(connection, node_rows, [])

            edge_rows = []
            for node_id, _, predecessor_ids in graph.iter_serialized_records(
                input_filename
            ):
                edge_rows.extend(
                    (predecessor_id, node_id) for predecessor_id in predecessor_ids
                )
                if len(edge_rows) >= _BATCH_SIZE:
                    _flush_batches(connection, [], edge_rows)
            _flush_batches(connection, [], edge_rows)

            connection.execute("CREATE INDEX edges_by_retainer ON edges(retainer_id)")
            connection.execute("CREATE INDEX edges_by_retained ON edges(retained_id)")
            connection.execute("CREATE INDEX nodes_by_kind_name ON nodes(kind, name)")
            connection.execute("PRAGMA user_version = %d" % SCHEMA_VERSION)
    except (OSError, ValueError, sqlite3.Error):
        if connection is not None:
            connection.close()
        _remove_partial_output(output_filename)
        raise
    else:
        connection.close()
