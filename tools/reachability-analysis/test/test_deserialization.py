# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import unittest

from lib import core


class TestGraphDeserialization(unittest.TestCase):
    def test_reachability_graph(self):
        """
        Check that we are able to recover the same graph serialized in
        ReachabilityGraphSerialization.cpp.
        """
        graph_file = os.environ["REACHABILITY_GRAPH_FILE"]
        graph = core.ReachabilityGraph()
        graph.load(graph_file)

        seed = graph.get_seed("<SEED>")
        cls = graph.get_node("LFoo;")
        anno = graph.get_anno("LAnno;")
        field = graph.get_node("LFoo;.field1:I")
        method = graph.get_node("LFoo;.method1:()I")

        def assertEdge(pred, succ):
            self.assertIn(succ, pred.succs)
            self.assertIn(pred, succ.preds)

        assertEdge(seed, cls)
        assertEdge(cls, anno)
        assertEdge(cls, method)
        assertEdge(method, field)

    def test_method_override_graph(self):
        """
        Check that we are able to recover the same graph serialized in
        MethodOverrideGraphSerialization.cpp.
        """
        graph_file = os.environ["METHOD_OVERRIDE_GRAPH_FILE"]
        graph = core.MethodOverrideGraph()
        graph.load(graph_file)

        m1 = graph.nodes["LFoo;.bar:()V"]
        m2 = graph.nodes["LBar;.bar:()V"]
        m3 = graph.nodes["LBaz;.bar:()V"]
        m4 = graph.nodes["LQux;.bar:()V"]

        def assertEdge(parent, child):
            self.assertIn(child, parent.children)
            self.assertIn(parent, child.parents)

        assertEdge(m1, m2)
        assertEdge(m1, m3)
        assertEdge(m2, m4)
        assertEdge(m3, m4)
