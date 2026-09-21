#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import sqlite3
from collections.abc import Iterator
from pathlib import Path

from . import analysis
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


def resolve_node(
    connection: sqlite3.Connection,
    name: str,
    kind: str | None = None,
    kind_option: str = "--kind",
) -> tuple[str, str]:
    if kind is None:
        matches = connection.execute(
            """
            SELECT DISTINCT kind, name
            FROM nodes INDEXED BY nodes_by_kind_name
            WHERE kind IN ('ANNO', 'CLASS', 'FIELD', 'METHOD', 'SEED')
              AND name = ?
            ORDER BY kind
            """,
            (name,),
        ).fetchall()
    else:
        matches = connection.execute(
            "SELECT DISTINCT kind, name FROM nodes "
            "WHERE kind = ? AND name = ? ORDER BY kind",
            (kind, name),
        ).fetchall()
    if not matches:
        qualifier = f"{kind} " if kind is not None else ""
        raise ValueError(f"No {qualifier}node named {name!r}")
    if len(matches) > 1:
        kinds = ", ".join(row["kind"] for row in matches)
        raise ValueError(
            f"Ambiguous node named {name!r} ({kinds}); "
            f"use {kind_option} to select one"
        )
    return matches[0]["kind"], matches[0]["name"]


def _neighbors_in_direction(
    connection: sqlite3.Connection,
    node: tuple[str, str],
    direction: str,
) -> Iterator[dict[str, object]]:
    if direction == "retainer":
        join = "edge.retained_id = selected.id"
        neighbor_id = "edge.retainer_id"
        index = "edges_by_retained"
    else:
        join = "edge.retainer_id = selected.id"
        neighbor_id = "edge.retained_id"
        index = "edges_by_retainer"
    cursor = connection.execute(
        f"""
        SELECT DISTINCT :direction AS direction,
               neighbor.kind AS type,
               neighbor.name AS name
        FROM nodes AS selected
        JOIN edges AS edge INDEXED BY {index} ON {join}
        JOIN nodes AS neighbor ON neighbor.id = {neighbor_id}
        WHERE selected.kind = :kind AND selected.name = :name
        ORDER BY neighbor.kind, neighbor.name
        """,
        {
            "direction": direction,
            "kind": node[0],
            "name": node[1],
        },
    )
    return _rows(cursor)


def neighbors(
    connection: sqlite3.Connection,
    node: tuple[str, str],
    direction: str,
) -> Iterator[dict[str, object]]:
    if direction in ("retainers", "both"):
        yield from _neighbors_in_direction(connection, node, "retainer")
    if direction in ("retained", "both"):
        yield from _neighbors_in_direction(connection, node, "retained")


def _create_path_tables(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        DROP TABLE IF EXISTS temp.path_seen;
        DROP TABLE IF EXISTS temp.path_frontier;
        DROP TABLE IF EXISTS temp.path_next;
        CREATE TEMP TABLE path_seen(
          kind TEXT NOT NULL,
          name TEXT NOT NULL,
          parent_kind TEXT,
          parent_name TEXT,
          PRIMARY KEY(kind, name)
        ) WITHOUT ROWID;
        CREATE TEMP TABLE path_frontier(
          kind TEXT NOT NULL,
          name TEXT NOT NULL,
          PRIMARY KEY(kind, name)
        ) WITHOUT ROWID;
        CREATE TEMP TABLE path_next(
          kind TEXT NOT NULL,
          name TEXT NOT NULL,
          parent_kind TEXT NOT NULL,
          parent_name TEXT NOT NULL,
          PRIMARY KEY(kind, name)
        ) WITHOUT ROWID;
        """
    )


def _advance_path_frontier(connection: sqlite3.Connection) -> bool:
    connection.execute("DELETE FROM path_next")
    connection.execute(
        """
        INSERT OR IGNORE INTO path_next(kind, name, parent_kind, parent_name)
        SELECT child.kind, child.name, parent.kind, parent.name
        FROM path_frontier AS frontier
        JOIN nodes AS parent
          ON parent.kind = frontier.kind AND parent.name = frontier.name
        JOIN edges AS edge INDEXED BY edges_by_retainer
          ON edge.retainer_id = parent.id
        JOIN nodes AS child ON child.id = edge.retained_id
        LEFT JOIN path_seen AS seen
          ON seen.kind = child.kind AND seen.name = child.name
        WHERE seen.kind IS NULL
        ORDER BY child.kind, child.name, parent.kind, parent.name
        """
    )
    has_next = connection.execute("SELECT EXISTS(SELECT 1 FROM path_next)").fetchone()[
        0
    ]
    connection.execute(
        """
        INSERT INTO path_seen(kind, name, parent_kind, parent_name)
        SELECT kind, name, parent_kind, parent_name FROM path_next
        """
    )
    connection.execute("DELETE FROM path_frontier")
    connection.execute(
        "INSERT INTO path_frontier(kind, name) SELECT kind, name FROM path_next"
    )
    return bool(has_next)


def shortest_path(
    connection: sqlite3.Connection,
    start: tuple[str, str],
    end: tuple[str, str],
) -> list[dict[str, object]] | None:
    _create_path_tables(connection)
    connection.execute(
        "INSERT INTO path_seen(kind, name) VALUES (?, ?)",
        start,
    )
    connection.execute(
        "INSERT INTO path_frontier(kind, name) VALUES (?, ?)",
        start,
    )
    while start != end:
        if not _advance_path_frontier(connection):
            return None
        if connection.execute(
            "SELECT 1 FROM path_seen WHERE kind = ? AND name = ?",
            end,
        ).fetchone():
            break

    path = []
    node: tuple[str, str] | None = end
    while node is not None:
        path.append(node)
        parent = connection.execute(
            "SELECT parent_kind, parent_name FROM path_seen "
            "WHERE kind = ? AND name = ?",
            node,
        ).fetchone()
        node = (
            None
            if parent["parent_kind"] is None
            else (parent["parent_kind"], parent["parent_name"])
        )
    path.reverse()
    return [
        {"status": "found", "step": step, "type": node[0], "name": node[1]}
        for step, node in enumerate(path)
    ]


def dominated(
    connection: sqlite3.Connection,
    blocked: tuple[str, str],
) -> Iterator[dict[str, object]]:
    cursor = connection.execute(
        """
        WITH RECURSIVE
        logical_nodes(kind, name) AS (
          SELECT DISTINCT kind, name FROM nodes
        ),
        reachable(kind, name) AS (
          SELECT node.kind, node.name
          FROM logical_nodes AS node
          WHERE NOT (node.kind = :blocked_kind AND node.name = :blocked_name)
            AND NOT EXISTS (
              SELECT 1
              FROM nodes AS raw
              JOIN edges AS edge INDEXED BY edges_by_retained
                ON edge.retained_id = raw.id
              WHERE raw.kind = node.kind AND raw.name = node.name
            )
          UNION
          SELECT retained.kind, retained.name
          FROM reachable
          JOIN nodes AS retainer
            ON retainer.kind = reachable.kind
           AND retainer.name = reachable.name
          JOIN edges AS edge INDEXED BY edges_by_retainer
            ON edge.retainer_id = retainer.id
          JOIN nodes AS retained ON retained.id = edge.retained_id
          WHERE NOT (
            retained.kind = :blocked_kind AND retained.name = :blocked_name
          )
        )
        SELECT node.kind AS type, node.name AS name
        FROM logical_nodes AS node
        WHERE NOT EXISTS (
          SELECT 1 FROM reachable
          WHERE reachable.kind = node.kind AND reachable.name = node.name
        )
        ORDER BY node.kind, node.name
        """,
        {
            "blocked_kind": blocked[0],
            "blocked_name": blocked[1],
        },
    )
    return _rows(cursor)


class _LogicalNode:
    __slots__ = ("kind", "name", "preds", "succs")

    def __init__(self, kind: str, name: str) -> None:
        self.kind = kind
        self.name = name
        self.preds: set[_LogicalNode] = set()
        self.succs: set[_LogicalNode] = set()


class _LogicalGraph:
    __slots__ = ("nodes",)

    def __init__(self, nodes: dict[tuple[str, str], _LogicalNode]) -> None:
        self.nodes = nodes


def _load_ancestor_graph(
    connection: sqlite3.Connection,
    target: tuple[str, str],
) -> _LogicalGraph:
    connection.executescript(
        """
        DROP TABLE IF EXISTS temp.dominator_nodes;
        CREATE TEMP TABLE dominator_nodes(
          kind TEXT NOT NULL,
          name TEXT NOT NULL,
          PRIMARY KEY(kind, name)
        ) WITHOUT ROWID;
        """
    )
    connection.execute(
        """
        WITH RECURSIVE ancestors(kind, name) AS (
          VALUES(:kind, :name)
          UNION
          SELECT predecessor.kind, predecessor.name
          FROM ancestors
          JOIN nodes AS retained
            ON retained.kind = ancestors.kind
           AND retained.name = ancestors.name
          JOIN edges AS edge INDEXED BY edges_by_retained
            ON edge.retained_id = retained.id
          JOIN nodes AS predecessor ON predecessor.id = edge.retainer_id
        )
        INSERT INTO dominator_nodes(kind, name)
        SELECT kind, name FROM ancestors
        """,
        {"kind": target[0], "name": target[1]},
    )

    nodes = {
        (row["kind"], row["name"]): _LogicalNode(row["kind"], row["name"])
        for row in connection.execute(
            "SELECT kind, name FROM dominator_nodes ORDER BY kind, name"
        )
    }
    for row in connection.execute(
        """
        SELECT DISTINCT
          retainer.kind AS retainer_kind,
          retainer.name AS retainer_name,
          retained.kind AS retained_kind,
          retained.name AS retained_name
        FROM edges AS edge
        JOIN nodes AS retainer ON retainer.id = edge.retainer_id
        JOIN dominator_nodes AS predecessor
          ON predecessor.kind = retainer.kind
         AND predecessor.name = retainer.name
        JOIN nodes AS retained ON retained.id = edge.retained_id
        JOIN dominator_nodes AS successor
          ON successor.kind = retained.kind
         AND successor.name = retained.name
        """
    ):
        predecessor = nodes[(row["retainer_kind"], row["retainer_name"])]
        successor = nodes[(row["retained_kind"], row["retained_name"])]
        predecessor.succs.add(successor)
        successor.preds.add(predecessor)
    return _LogicalGraph(nodes)


def dominators(
    connection: sqlite3.Connection,
    target: tuple[str, str],
) -> list[dict[str, object]]:
    graph = _load_ancestor_graph(connection, target)
    result = analysis.get_dominators(graph, graph.nodes[target])
    return [{"type": node.kind, "name": node.name} for node in result]


def new_nodes(
    connection: sqlite3.Connection,
    kind: str | None,
) -> Iterator[dict[str, object]]:
    marker_exists = connection.execute(
        "SELECT 1 FROM nodes WHERE kind = 'ANNO' AND name = '<NEW>' LIMIT 1"
    ).fetchone()
    if marker_exists is None:
        raise ValueError("input is not a reachability-diff graph: no <NEW> marker")

    cursor = connection.execute(
        """
        SELECT DISTINCT node.kind AS type, node.name AS name
        FROM nodes AS marker
        JOIN edges AS edge INDEXED BY edges_by_retained
          ON edge.retained_id = marker.id
        JOIN nodes AS node ON node.id = edge.retainer_id
        WHERE marker.kind = 'ANNO' AND marker.name = '<NEW>'
          AND (:kind IS NULL OR node.kind = :kind)
        ORDER BY node.kind, node.name
        """,
        {"kind": kind},
    )
    return _rows(cursor)
