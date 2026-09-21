# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from lib.core import ReachabilityGraph, ReachableObject, ReachableObjectType
from lib.reachability_diff import NEW_NODE_MARKER


def write_diff_graph(filename):
    graph = ReachabilityGraph()
    marker = ReachableObject(ReachableObjectType.ANNO, NEW_NODE_MARKER)
    graph.add_node(marker)

    for node_type, name in (
        (ReachableObjectType.CLASS, "LNewClass;"),
        (ReachableObjectType.FIELD, "LNewClass;.field:I"),
        (ReachableObjectType.METHOD, "LNewClass;.method:()V"),
        (ReachableObjectType.ANNO, "LNewAnno;"),
    ):
        node = ReachableObject(node_type, name)
        graph.add_node(node)
        graph.add_edge(marker, node)

    graph.add_node(ReachableObject(ReachableObjectType.CLASS, "LUnrelated;"))
    graph.dump(filename)
