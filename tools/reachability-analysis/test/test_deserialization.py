# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from lib import core

import os
import unittest


class TestGraphDeserialization(unittest.TestCase):
    def test_simple(self):
        """
        Check that we are able to recover the same graph serialized in
        SimpleGraphSerialization.cpp.
        """
        graph_file = os.environ["GRAPH_FILE"]
        graph = core.Graph.load(graph_file)

        seed = graph.get_seed("<SEED>")
        cls = graph.get_node("LFoo;")
        anno = graph.get_anno("LAnno;")
        field = graph.get_node("LFoo;.field1:I")
        method = graph.get_node("LFoo;.method1:()I")
        self.assertEdge(seed, cls)
        self.assertEdge(cls, anno)
        self.assertEdge(cls, method)
        self.assertEdge(method, field)

    def assertEdge(self, pred, succ):
        self.assertIn(succ, pred.succs)
        self.assertIn(pred, succ.preds)
