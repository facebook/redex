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

from lib.core import ReachabilityGraph, ReachableObject, ReachableObjectType
from lib.sqlite_export import export_graph

from .test_utils import write_diff_graph


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

    def _write_graph_db(self, graph, directory):
        graph_file = os.path.join(directory, "reachability.graph")
        db_file = os.path.join(directory, "reachability.sqlite")
        graph.dump(graph_file)
        export_graph(graph_file, db_file)
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
            "Error: Unsupported reachability SQLite schema version 0; expected 2\n",
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

    def test_semantic_roots_resolves_seed_reasons_and_excludes_non_program_nodes(
        self,
    ):
        output = self._run("json", "semantic-roots")

        self.assertEqual(
            [
                {"type": "CLASS", "name": "LFoo;", "reason": "<SEED>"},
                {"type": "CLASS", "name": "LRemovedRoot;", "reason": ""},
            ],
            json.loads(output),
        )

    def test_neighbors_labels_both_edge_directions(self):
        output = self._run("csv", "neighbors", "LFoo;")

        self.assertEqual(
            [
                {"direction": "retainer", "type": "SEED", "name": "<SEED>"},
                {"direction": "retained", "type": "ANNO", "name": "LAnno;"},
                {
                    "direction": "retained",
                    "type": "METHOD",
                    "name": "LFoo;.method1:()I",
                },
            ],
            list(csv.DictReader(io.StringIO(output))),
        )

    def test_neighbors_resolves_annotation_names(self):
        output = self._run("json", "neighbors", "LAnno;", "--direction", "retainers")

        self.assertEqual(
            [{"direction": "retainer", "type": "CLASS", "name": "LFoo;"}],
            json.loads(output),
        )

    def test_table_format_and_missing_node_error(self):
        output = self._run("table", "neighbors", "LFoo;", "--direction", "retainers")
        self.assertIn("direction", output)
        self.assertIn("retainer", output)
        self.assertIn("<SEED>", output)

        result = self._invoke(self.db_file, "table", "neighbors", "LMissing;")
        self.assertEqual(1, result.returncode)
        self.assertEqual("Error: No node named 'LMissing;'\n", result.stderr)

    def test_neighbors_disambiguates_class_and_annotation_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            class_node = ReachableObject(ReachableObjectType.CLASS, "LCollision;")
            annotation_node = ReachableObject(ReachableObjectType.ANNO, "LCollision;")
            annotation_retainer = ReachableObject(
                ReachableObjectType.CLASS, "LAnnotationRetainer;"
            )
            for node in (class_node, annotation_node, annotation_retainer):
                graph.add_node(node)
            graph.add_edge(annotation_node, annotation_retainer)
            db_file = self._write_graph_db(graph, temp_dir)

            result = self._invoke(
                db_file,
                "json",
                "neighbors",
                "LCollision;",
                "--kind",
                "anno",
                "--direction",
                "retainers",
            )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            [
                {
                    "direction": "retainer",
                    "type": "CLASS",
                    "name": "LAnnotationRetainer;",
                }
            ],
            json.loads(result.stdout),
        )

    def test_path_follows_retainer_to_retained(self):
        output = self._run("json", "path", "<SEED>", "LFoo;.field1:I")
        rows = json.loads(output)

        self.assertEqual(
            [
                "<SEED>",
                "LFoo;",
                "LFoo;.method1:()I",
                "LFoo;.field1:I",
            ],
            [row["name"] for row in rows],
        )
        self.assertEqual([0, 1, 2, 3], [row["step"] for row in rows])

    def test_path_reports_no_path(self):
        self.assertEqual(
            [{"status": "no path", "step": "", "type": "", "name": ""}],
            json.loads(self._run("json", "path", "LFoo;.field1:I", "<SEED>")),
        )

    def test_path_disambiguates_both_endpoint_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            start = ReachableObject(ReachableObjectType.CLASS, "LCollision;")
            target = ReachableObject(ReachableObjectType.ANNO, "LTarget;")
            for node in (
                start,
                ReachableObject(ReachableObjectType.ANNO, "LCollision;"),
                ReachableObject(ReachableObjectType.CLASS, "LTarget;"),
                target,
            ):
                graph.add_node(node)
            graph.add_edge(target, start)
            db_file = self._write_graph_db(graph, temp_dir)

            result = self._invoke(
                db_file,
                "json",
                "path",
                "LCollision;",
                "LTarget;",
                "--from-kind",
                "class",
                "--to-kind",
                "anno",
            )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            [
                {
                    "status": "found",
                    "step": 0,
                    "type": "CLASS",
                    "name": "LCollision;",
                },
                {
                    "status": "found",
                    "step": 1,
                    "type": "ANNO",
                    "name": "LTarget;",
                },
            ],
            json.loads(result.stdout),
        )

    def test_dominated_disambiguates_class_and_annotation_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            root = ReachableObject(ReachableObjectType.SEED, "<SEED>")
            selected = ReachableObject(ReachableObjectType.CLASS, "LCollision;")
            child = ReachableObject(ReachableObjectType.CLASS, "LClassChild;")
            other = ReachableObject(ReachableObjectType.ANNO, "LCollision;")
            for node in (root, selected, child, other):
                graph.add_node(node)
            graph.add_edge(selected, root)
            graph.add_edge(child, selected)
            graph.add_edge(other, root)
            db_file = self._write_graph_db(graph, temp_dir)

            result = self._invoke(
                db_file,
                "json",
                "dominated",
                "LCollision;",
                "--kind",
                "class",
            )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            [
                {"type": "CLASS", "name": "LClassChild;"},
                {"type": "CLASS", "name": "LCollision;"},
            ],
            json.loads(result.stdout),
        )

    def test_dominated_returns_selected_node_and_dominated_set(self):
        output = self._run("json", "dominated", "LRemovedRoot;")

        self.assertCountEqual(
            [
                {"type": "CLASS", "name": "LRemovedRoot;"},
                {"type": "CLASS", "name": "LRemovedChild;"},
            ],
            json.loads(output),
        )

    def test_dominated_terminates_on_cycles(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            root = ReachableObject(ReachableObjectType.SEED, "<SEED>")
            selected = ReachableObject(ReachableObjectType.CLASS, "selected")
            child = ReachableObject(ReachableObjectType.CLASS, "child")
            for node in (root, selected, child):
                graph.add_node(node)
            graph.add_edge(selected, root)
            graph.add_edge(child, selected)
            graph.add_edge(selected, child)
            db_file = self._write_graph_db(graph, temp_dir)

            output = self._run_db(db_file, "json", "dominated", "selected")

        self.assertEqual(
            [
                {"type": "CLASS", "name": "child"},
                {"type": "CLASS", "name": "selected"},
            ],
            json.loads(output),
        )

    def test_dominators_disambiguates_class_and_annotation_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            root = ReachableObject(ReachableObjectType.SEED, "<SEED>")
            selected_retainer = ReachableObject(
                ReachableObjectType.CLASS, "LAnnotationRetainer;"
            )
            selected = ReachableObject(ReachableObjectType.ANNO, "LCollision;")
            other_retainer = ReachableObject(
                ReachableObjectType.CLASS, "LClassRetainer;"
            )
            other = ReachableObject(ReachableObjectType.CLASS, "LCollision;")
            for node in (
                root,
                selected_retainer,
                selected,
                other_retainer,
                other,
            ):
                graph.add_node(node)
            graph.add_edge(selected_retainer, root)
            graph.add_edge(selected, selected_retainer)
            graph.add_edge(other_retainer, root)
            graph.add_edge(other, other_retainer)
            db_file = self._write_graph_db(graph, temp_dir)

            result = self._invoke(
                db_file,
                "json",
                "dominators",
                "LCollision;",
                "--kind",
                "anno",
            )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            [
                {"type": "CLASS", "name": "LAnnotationRetainer;"},
                {"type": "SEED", "name": "<SEED>"},
            ],
            json.loads(result.stdout),
        )

    def test_dominators_returns_proper_chain_nearest_first(self):
        output = self._run("json", "dominators", "LFoo;.field1:I")

        self.assertEqual(
            [
                {"type": "METHOD", "name": "LFoo;.method1:()I"},
                {"type": "CLASS", "name": "LFoo;"},
                {"type": "SEED", "name": "<SEED>"},
            ],
            json.loads(output),
        )

    def test_dominators_reports_a_target_unreachable_from_roots(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph = ReachabilityGraph()
            first = ReachableObject(ReachableObjectType.CLASS, "first")
            target = ReachableObject(ReachableObjectType.CLASS, "target")
            graph.add_node(first)
            graph.add_node(target)
            graph.add_edge(target, first)
            graph.add_edge(first, target)
            db_file = self._write_graph_db(graph, temp_dir)

            result = self._invoke(db_file, "table", "dominators", "target")

        self.assertEqual(1, result.returncode)
        self.assertIn("target", result.stderr)
        self.assertIn("not reachable from any root", result.stderr)

    def test_new_nodes_returns_marker_predecessors_and_filters_kind(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            graph_file = os.path.join(temp_dir, "diff.graph")
            db_file = os.path.join(temp_dir, "diff.sqlite")
            write_diff_graph(graph_file)
            export_graph(graph_file, db_file)

            result = self._invoke(db_file, "json", "new-nodes")
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(
                [
                    {"type": "ANNO", "name": "LNewAnno;"},
                    {"type": "CLASS", "name": "LNewClass;"},
                    {"type": "FIELD", "name": "LNewClass;.field:I"},
                    {"type": "METHOD", "name": "LNewClass;.method:()V"},
                ],
                json.loads(result.stdout),
            )

            filtered = self._invoke(
                db_file,
                "json",
                "new-nodes",
                "--kind",
                "class",
            )
            self.assertEqual(0, filtered.returncode, filtered.stderr)
            self.assertEqual(
                [{"type": "CLASS", "name": "LNewClass;"}],
                json.loads(filtered.stdout),
            )

    def test_new_nodes_rejects_a_non_diff_graph(self):
        result = self._invoke(self.db_file, "table", "new-nodes")
        self.assertEqual(1, result.returncode)
        self.assertEqual(
            "Error: input is not a reachability-diff graph: no <NEW> marker\n",
            result.stderr,
        )
