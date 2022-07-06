# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest

from lib import analysis, core
from lib.core import ReachableObject, ReachableObjectType


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
