# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest

from lib import analysis, core
from lib.core import ReachableObject, ReachableObjectType


def make_graph(node_names, edges):
    graph = core.ReachabilityGraph()
    nodes = {
        name: ReachableObject(ReachableObjectType.CLASS, name) for name in node_names
    }
    for node in nodes.values():
        graph.add_node(node)
    for retainer, retained in edges:
        graph.add_edge(nodes[retained], nodes[retainer])
    return graph, nodes


class TestAnalysis(unittest.TestCase):
    def test_group_by_common_keys(self):
        d1 = {"k1": {"v1", "v2", "v3"}, "k2": {"v1", "v2"}, "k3": {"v1", "v2"}}
        d1_expected = {
            frozenset(["k1", "k2", "k3"]): {"v1", "v2"},
            frozenset(["k1"]): {"v3"},
        }
        self.assertDictEqual(analysis.group_by_common_keys(d1), d1_expected)

    def test_find_boundary(self):
        graph = core.ReachabilityGraph()
        foo = ReachableObject(ReachableObjectType.CLASS, "LFoo;")
        bar = ReachableObject(ReachableObjectType.CLASS, "LBar;")
        qux = ReachableObject(ReachableObjectType.CLASS, "LQux;")
        corge = ReachableObject(ReachableObjectType.CLASS, "LCorge;")
        anno = ReachableObject(ReachableObjectType.ANNO, "LJsonSerialize;")
        graph.add_node(foo)
        graph.add_node(bar)
        graph.add_node(qux)
        graph.add_node(corge)
        graph.add_node(anno)

        graph.add_edge(bar, foo)
        graph.add_edge(bar, qux)
        graph.add_edge(corge, bar)
        # Check that we are ignoring annotations that point back into the query
        # set.
        graph.add_edge(bar, anno)
        graph.add_edge(anno, bar)

        self.assertDictEqual(
            analysis.find_boundary(graph, {bar, corge}), {foo: {bar}, qux: {bar}}
        )
        self.assertDictEqual(
            analysis.find_boundary(graph, {bar}), {foo: {bar}, qux: {bar}}
        )
        self.assertDictEqual(analysis.find_boundary(graph, {corge}), {bar: {corge}})

    def test_group_members_by_class(self):
        graph = core.ReachabilityGraph()
        foo = ReachableObject(ReachableObjectType.CLASS, "LFoo;")
        foo_bar = ReachableObject(ReachableObjectType.FIELD, "LFoo;.bar:I")
        foo_baz = ReachableObject(ReachableObjectType.METHOD, "LFoo;.baz:()I")
        graph.add_node(foo)
        graph.add_node(foo_bar)
        graph.add_node(foo_baz)

        grouped = analysis.group_members_by_class(graph)
        self.assertDictEqual(grouped, {foo: {foo_bar, foo_baz}})

    def test_get_dominated_handles_a_deep_graph(self):
        graph = core.ReachabilityGraph()
        nodes = [
            ReachableObject(ReachableObjectType.CLASS, f"node{index}")
            for index in range(1100)
        ]
        for node in nodes:
            graph.add_node(node)
        for retainer, retained in zip(nodes, nodes[1:]):
            graph.add_edge(retained, retainer)

        self.assertSetEqual(
            {nodes[-1]},
            analysis.get_dominated(graph, {nodes[-1]}),
        )

    def test_get_dominators_on_linear_chain(self):
        graph, nodes = make_graph(
            ("root", "a", "b", "target"),
            (("root", "a"), ("a", "b"), ("b", "target")),
        )

        self.assertEqual(
            [nodes["b"], nodes["a"], nodes["root"]],
            analysis.get_dominators(graph, nodes["target"]),
        )

    def test_get_dominators_keeps_only_shared_nodes_across_roots(self):
        graph, nodes = make_graph(
            ("root1", "root2", "left", "right", "shared", "target"),
            (
                ("root1", "left"),
                ("root2", "right"),
                ("left", "shared"),
                ("right", "shared"),
                ("shared", "target"),
            ),
        )

        self.assertEqual(
            [nodes["shared"]],
            analysis.get_dominators(graph, nodes["target"]),
        )

    def test_get_dominators_handles_a_reachable_cycle(self):
        graph, nodes = make_graph(
            ("root", "a", "b", "target"),
            (
                ("root", "a"),
                ("a", "b"),
                ("b", "a"),
                ("b", "target"),
            ),
        )

        self.assertEqual(
            [nodes["b"], nodes["a"], nodes["root"]],
            analysis.get_dominators(graph, nodes["target"]),
        )

    def test_get_dominators_rejects_a_target_unreachable_from_roots(self):
        graph, nodes = make_graph(
            ("a", "target"),
            (("a", "target"), ("target", "a")),
        )

        with self.assertRaisesRegex(ValueError, "not reachable from any root"):
            analysis.get_dominators(graph, nodes["target"])
