#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import sqlite3
from collections.abc import Iterator
from pathlib import Path

from .sqlite_export import EDGE_DIRECTION, SCHEMA_VERSION


SOURCE_FORMAT = "Redex reachability graph binary v1"
KINDS = ("ANNO", "CLASS", "FIELD", "METHOD", "SEED")


def _validate_database(connection: sqlite3.Connection) -> None:
    version = connection.execute("PRAGMA user_version").fetchone()[0]
    if version != SCHEMA_VERSION:
        raise ValueError(
            "Unsupported reachability SQLite schema version "
            f"{version}; expected {SCHEMA_VERSION}"
        )

    metadata = dict(
        connection.execute(
            "SELECT key, value FROM meta "
            "WHERE key IN ('edge_direction', 'source_format')"
        )
    )
    expected = {
        "edge_direction": EDGE_DIRECTION,
        "source_format": SOURCE_FORMAT,
    }
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ValueError(f"Invalid reachability SQLite metadata: {key}")


def open_database(filename: str) -> sqlite3.Connection:
    uri = f"{Path(filename).resolve().as_uri()}?mode=ro"
    connection = sqlite3.connect(uri, uri=True)
    connection.row_factory = sqlite3.Row
    try:
        _validate_database(connection)
    except (sqlite3.Error, ValueError):
        connection.close()
        raise
    return connection


def _rows(cursor: sqlite3.Cursor) -> Iterator[dict[str, object]]:
    for row in cursor:
        yield dict(row)


def roots(
    connection: sqlite3.Connection,
    kind: str | None,
    limit: int | None,
) -> Iterator[dict[str, object]]:
    cursor = connection.execute(
        """
        WITH logical_nodes(kind, name) AS (
          SELECT DISTINCT kind, name FROM nodes
        )
        SELECT node.kind AS type, node.name AS name
        FROM logical_nodes AS node
        WHERE (:kind IS NULL OR node.kind = :kind)
          AND NOT EXISTS (
            SELECT 1
            FROM nodes AS raw
            JOIN edges AS edge INDEXED BY edges_by_retained
              ON edge.retained_id = raw.id
            WHERE raw.kind = node.kind AND raw.name = node.name
          )
        ORDER BY node.kind, node.name
        LIMIT :limit
        """,
        {
            "kind": kind,
            "limit": -1 if limit is None else limit,
        },
    )
    return _rows(cursor)


def semantic_roots(
    connection: sqlite3.Connection,
) -> Iterator[dict[str, object]]:
    cursor = connection.execute(
        """
        WITH logical_nodes(kind, name) AS (
          SELECT DISTINCT kind, name FROM nodes
        ),
        seed_roots(type, name, reason) AS (
          SELECT DISTINCT target.kind, target.name, seed.name
          FROM nodes AS target
          JOIN edges AS edge INDEXED BY edges_by_retained
            ON edge.retained_id = target.id
          JOIN nodes AS seed ON seed.id = edge.retainer_id
          WHERE target.kind IN ('CLASS', 'FIELD', 'METHOD')
            AND seed.kind = 'SEED'
        ),
        literal_roots(type, name, reason) AS (
          SELECT node.kind, node.name, ''
          FROM logical_nodes AS node
          WHERE node.kind IN ('CLASS', 'FIELD', 'METHOD')
            AND NOT EXISTS (
              SELECT 1
              FROM nodes AS raw
              JOIN edges AS edge INDEXED BY edges_by_retained
                ON edge.retained_id = raw.id
              WHERE raw.kind = node.kind AND raw.name = node.name
            )
        )
        SELECT type, name, reason FROM seed_roots
        UNION
        SELECT type, name, reason FROM literal_roots
        ORDER BY type, name, reason
        """
    )
    return _rows(cursor)
