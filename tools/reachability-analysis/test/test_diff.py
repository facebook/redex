# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest

from lib.core import ReachabilityGraph, ReachableObject, ReachableObjectType
from lib.reachability_diff import build_diff_graph, NEW_NODE_MARKER


def add_node(graph, node_type, name):
    node = ReachableObject(node_type, name)
    graph.add_node(node)
    return node


def keys(graph):
    return set(graph.nodes)


class ReachabilityDiffTest(unittest.TestCase):
    def test_keeps_rooted_ancestors_and_descendants_of_new_nodes(self):
        old_graph = ReachabilityGraph()
        old_root = add_node(old_graph, ReachableObjectType.SEED, "old root")
        old_ancestor = add_node(old_graph, ReachableObjectType.CLASS, "LAncestor;")
        old_descendant = add_node(old_graph, ReachableObjectType.CLASS, "LDescendant;")
        add_node(old_graph, ReachableObjectType.CLASS, "LUnrelated;")
        old_graph.add_edge(old_ancestor, old_root)
        old_graph.add_edge(old_descendant, old_ancestor)

        new_graph = ReachabilityGraph()
        root = add_node(new_graph, ReachableObjectType.SEED, "old root")
        ancestor = add_node(new_graph, ReachableObjectType.CLASS, "LAncestor;")
        new_node = add_node(new_graph, ReachableObjectType.CLASS, "LNew;")
        descendant = add_node(new_graph, ReachableObjectType.CLASS, "LDescendant;")
        unrelated = add_node(new_graph, ReachableObjectType.CLASS, "LUnrelated;")
        new_graph.add_edge(ancestor, root)
        new_graph.add_edge(new_node, ancestor)
        new_graph.add_edge(descendant, new_node)
        new_graph.add_edge(unrelated, root)

        result = build_diff_graph(old_graph, new_graph)

        self.assertEqual(
            {
                (ReachableObjectType.ANNO, NEW_NODE_MARKER),
                (ReachableObjectType.CLASS, "LAncestor;"),
                (ReachableObjectType.CLASS, "LDescendant;"),
                (ReachableObjectType.CLASS, "LNew;"),
                (ReachableObjectType.SEED, "old root"),
            },
            keys(result),
        )
        self.assertNotIn((ReachableObjectType.CLASS, "LUnrelated;"), result.nodes)

        copied_new = result.get_node("LNew;")
        marker = result.get_anno(NEW_NODE_MARKER)
        self.assertEqual({copied_new}, set(marker.preds))
        self.assertIn(marker, copied_new.succs)
        self.assertIn(result.get_node("LAncestor;"), copied_new.preds)
        self.assertIn(result.get_node("LDescendant;"), copied_new.succs)

    def test_excludes_predecessors_without_a_path_from_a_root(self):
        old_graph = ReachabilityGraph()
        old_root = add_node(old_graph, ReachableObjectType.SEED, "root")
        old_ancestor = add_node(old_graph, ReachableObjectType.CLASS, "LAncestor;")
        old_cycle_a = add_node(old_graph, ReachableObjectType.CLASS, "LCycleA;")
        old_cycle_b = add_node(old_graph, ReachableObjectType.CLASS, "LCycleB;")
        old_graph.add_edge(old_ancestor, old_root)
        old_graph.add_edge(old_cycle_b, old_cycle_a)
        old_graph.add_edge(old_cycle_a, old_cycle_b)

        new_graph = ReachabilityGraph()
        root = add_node(new_graph, ReachableObjectType.SEED, "root")
        ancestor = add_node(new_graph, ReachableObjectType.CLASS, "LAncestor;")
        new_node = add_node(new_graph, ReachableObjectType.CLASS, "LNew;")
        cycle_a = add_node(new_graph, ReachableObjectType.CLASS, "LCycleA;")
        cycle_b = add_node(new_graph, ReachableObjectType.CLASS, "LCycleB;")
        new_graph.add_edge(ancestor, root)
        new_graph.add_edge(new_node, ancestor)
        new_graph.add_edge(cycle_b, cycle_a)
        new_graph.add_edge(cycle_a, cycle_b)
        new_graph.add_edge(new_node, cycle_a)

        result = build_diff_graph(old_graph, new_graph)

        self.assertNotIn((ReachableObjectType.CLASS, "LCycleA;"), result.nodes)
        self.assertNotIn((ReachableObjectType.CLASS, "LCycleB;"), result.nodes)
        self.assertIn((ReachableObjectType.CLASS, "LNew;"), result.nodes)

    def test_duplicate_labels_contribute_all_edges(self):
        old_graph = ReachabilityGraph()
        add_node(old_graph, ReachableObjectType.SEED, "root")
        add_node(old_graph, ReachableObjectType.ANNO, "LAnno;")

        new_graph = ReachabilityGraph()
        root = add_node(new_graph, ReachableObjectType.SEED, "root")
        first_anno = add_node(new_graph, ReachableObjectType.ANNO, "LAnno;")
        add_node(new_graph, ReachableObjectType.ANNO, "LAnno;")
        new_node = add_node(new_graph, ReachableObjectType.CLASS, "LNew;")
        new_graph.add_edge(first_anno, root)
        new_graph.add_edge(new_node, first_anno)

        result = build_diff_graph(old_graph, new_graph)

        self.assertIn((ReachableObjectType.SEED, "root"), result.nodes)
        self.assertIn((ReachableObjectType.ANNO, "LAnno;"), result.nodes)
        copied_anno = result.get_anno("LAnno;")
        self.assertIn(result.get_seed("root"), copied_anno.preds)
        self.assertIn(result.get_node("LNew;"), copied_anno.succs)

    def test_empty_diff_produces_empty_graph(self):
        old_graph = ReachabilityGraph()
        add_node(old_graph, ReachableObjectType.CLASS, "LFoo;")
        new_graph = ReachabilityGraph()
        add_node(new_graph, ReachableObjectType.CLASS, "LFoo;")

        result = build_diff_graph(old_graph, new_graph)

        self.assertEqual({}, result.nodes)
        self.assertEqual([], result.node_records)
