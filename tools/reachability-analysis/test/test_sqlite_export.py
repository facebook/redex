# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import sqlite3
import tempfile
import unittest

from lib import sqlite_export


class SqliteExportTest(unittest.TestCase):
    def test_exports_faithful_graph_with_named_edge_direction(self):
        graph_file = os.environ["REACHABILITY_GRAPH_FILE"]
        with tempfile.TemporaryDirectory() as temp_dir:
            output_file = os.path.join(temp_dir, "graph.sqlite")
            sqlite_export.export_graph(graph_file, output_file)

            connection = sqlite3.connect(output_file)
            try:
                self.assertEqual(
                    connection.execute("PRAGMA user_version").fetchone()[0], 1
                )
                self.assertEqual(
                    dict(connection.execute("SELECT key, value FROM meta")),
                    {
                        "schema_version": "1",
                        "source_graph": os.path.abspath(graph_file),
                        "source_format": "Redex reachability graph binary v1",
                        "edge_direction": sqlite_export.EDGE_DIRECTION,
                    },
                )
                self.assertEqual(
                    connection.execute("SELECT count(*) FROM nodes").fetchone()[0], 7
                )
                self.assertEqual(
                    connection.execute("SELECT count(*) FROM edges").fetchone()[0], 5
                )
                self.assertCountEqual(
                    connection.execute(
                        """
                        SELECT retainer.name, retained.name
                        FROM edges
                        JOIN nodes AS retainer ON retainer.id = edges.retainer_id
                        JOIN nodes AS retained ON retained.id = edges.retained_id
                        """
                    ),
                    [
                        ("<SEED>", "LFoo;"),
                        ("LFoo;", "LAnno;"),
                        ("LFoo;", "LFoo;.method1:()I"),
                        ("LFoo;.method1:()I", "LFoo;.field1:I"),
                        ("LRemovedRoot;", "LRemovedChild;"),
                    ],
                )
                self.assertCountEqual(
                    connection.execute(
                        """
                        SELECT name, kind
                        FROM nodes
                        WHERE id NOT IN (SELECT retained_id FROM edges)
                        """
                    ),
                    [("<SEED>", "SEED"), ("LRemovedRoot;", "CLASS")],
                )
                self.assertEqual(
                    connection.execute(
                        """
                        SELECT retainer.name
                        FROM edges
                        JOIN nodes AS retainer
                          ON retainer.id = edges.retainer_id
                        JOIN nodes AS retained
                          ON retained.id = edges.retained_id
                        WHERE retained.name = 'LFoo;' AND retainer.kind = 'SEED'
                        """
                    ).fetchall(),
                    [("<SEED>",)],
                )
                self.assertTrue(
                    {"edges_by_retainer", "edges_by_retained"}.issubset(
                        {
                            row[1]
                            for row in connection.execute("PRAGMA index_list(edges)")
                        }
                    )
                )
            finally:
                connection.close()

    def test_refuses_to_overwrite_output(self):
        graph_file = os.environ["REACHABILITY_GRAPH_FILE"]
        with tempfile.NamedTemporaryFile() as output_file:
            with self.assertRaisesRegex(FileExistsError, "already exists"):
                sqlite_export.export_graph(graph_file, output_file.name)
