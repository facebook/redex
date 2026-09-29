# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import csv
import io
import json
import os
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

from lib.sqlite_export import export_graph


class GraphQueryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._temp_dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._temp_dir.cleanup)
        cls.db_file = os.path.join(cls._temp_dir.name, "reachability.sqlite")
        export_graph(os.environ["REACHABILITY_GRAPH_FILE"], cls.db_file)

    def _invoke(self, db_file, output_format, *query_args):
        return subprocess.run(
            [
                os.environ["GRAPH_QUERY"],
                "--db",
                db_file,
                "--format",
                output_format,
                *query_args,
            ],
            capture_output=True,
            text=True,
        )

    def _run_db(self, db_file, output_format, *query_args):
        result = self._invoke(db_file, output_format, *query_args)
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout

    def _run(self, output_format, *query_args):
        return self._run_db(self.db_file, output_format, *query_args)

    def _copy_db(self, directory):
        db_file = os.path.join(directory, "reachability.sqlite")
        shutil.copyfile(self.db_file, db_file)
        return db_file

    def test_roots_filters_by_kind_and_limit(self):
        output = self._run("json", "roots", "--kind", "class", "--limit", "1")

        self.assertEqual(
            [{"type": "CLASS", "name": "LRemovedRoot;"}], json.loads(output)
        )

    def test_roots_supports_csv_and_table_output(self):
        csv_output = self._run("csv", "roots", "--kind", "class", "--limit", "1")
        self.assertEqual(
            [{"type": "CLASS", "name": "LRemovedRoot;"}],
            list(csv.DictReader(io.StringIO(csv_output))),
        )

        table_output = self._run("table", "roots", "--kind", "class", "--limit", "1")
        self.assertIn("type", table_output)
        self.assertIn("CLASS", table_output)
        self.assertIn("LRemovedRoot;", table_output)

    def test_roots_collapse_duplicate_logical_nodes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            db_file = self._copy_db(temp_dir)
            with sqlite3.connect(db_file) as connection:
                duplicate_id = connection.execute(
                    "SELECT MAX(id) + 1 FROM nodes"
                ).fetchone()[0]
                seed_id = connection.execute(
                    "SELECT id FROM nodes WHERE kind = 'SEED' AND name = '<SEED>'"
                ).fetchone()[0]
                connection.execute(
                    "INSERT INTO nodes(id, kind, name) VALUES (?, 'CLASS', ?)",
                    (duplicate_id, "LRemovedRoot;"),
                )
                connection.execute(
                    "INSERT INTO edges(retainer_id, retained_id) VALUES (?, ?)",
                    (seed_id, duplicate_id),
                )

            output = self._run_db(db_file, "json", "roots", "--kind", "class")

        self.assertEqual([], json.loads(output))

    def test_rejects_invalid_catalog_metadata(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            db_file = self._copy_db(temp_dir)
            with sqlite3.connect(db_file) as connection:
                connection.execute(
                    "UPDATE meta SET value = 'backwards' WHERE key = 'edge_direction'"
                )

            result = self._invoke(db_file, "json", "roots")

        self.assertEqual(1, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(
            "Error: Invalid reachability SQLite metadata: edge_direction\n",
            result.stderr,
        )

    def test_reports_catalog_errors_without_a_traceback(self):
        with tempfile.NamedTemporaryFile() as db_file:
            result = subprocess.run(
                [
                    os.environ["GRAPH_QUERY"],
                    "--db",
                    db_file.name,
                    "roots",
                ],
                capture_output=True,
                text=True,
            )

        self.assertEqual(1, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(
            "Error: Unsupported reachability SQLite schema version 0; expected 1\n",
            result.stderr,
        )

    def test_help_exposes_only_sqlite_input(self):
        result = subprocess.run(
            [os.environ["GRAPH_QUERY"], "--help"],
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--db", result.stdout)
        self.assertNotIn("--input", result.stdout)
