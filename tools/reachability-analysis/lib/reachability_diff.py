#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

from collections.abc import Mapping, Set

from .core import ReachabilityGraph, ReachableObject, ReachableObjectType

NEW_NODE_MARKER = "<NEW>"

NodeKey = tuple[int, str]


def _node_key(node: ReachableObject) -> NodeKey:
    return (node.type, node.name)


def newly_retained_keys(
    old_graph: ReachabilityGraph, new_graph: ReachabilityGraph
) -> set[NodeKey]:
    return set(new_graph.nodes) - set(old_graph.nodes)


def _logical_adjacency(
    graph: ReachabilityGraph,
) -> tuple[dict[NodeKey, set[NodeKey]], dict[NodeKey, set[NodeKey]]]:
    keys = set(graph.nodes)
    predecessors = {key: set() for key in keys}
    successors = {key: set() for key in keys}

    for node in graph.node_records:
        node_key = _node_key(node)
        for predecessor in node.preds:
            predecessor_key = _node_key(predecessor)
            if node_key not in keys or predecessor_key not in keys:
                continue
            predecessors[node_key].add(predecessor_key)
            successors[predecessor_key].add(node_key)
    return predecessors, successors


def _closure(
    starts: Set[NodeKey],
    adjacency: Mapping[NodeKey, Set[NodeKey]],
    allowed: Set[NodeKey] | None = None,
) -> set[NodeKey]:
    visited = set()
    pending = list(starts)
    while pending:
        node = pending.pop()
        if node in visited or (allowed is not None and node not in allowed):
            continue
        visited.add(node)
        pending.extend(adjacency[node])
    return visited


def build_diff_graph(
    old_graph: ReachabilityGraph, new_graph: ReachabilityGraph
) -> ReachabilityGraph:
    new_keys = newly_retained_keys(old_graph, new_graph)
    output = ReachabilityGraph()
    if not new_keys:
        return output

    marker_key = (ReachableObjectType.ANNO, NEW_NODE_MARKER)
    if marker_key in new_graph.nodes:
        raise ValueError(f"Input graph already contains marker node {marker_key}")

    predecessors, successors = _logical_adjacency(new_graph)
    ancestor_candidates = _closure(new_keys, predecessors)
    roots = {key for key in ancestor_candidates if not predecessors[key]}
    rooted_ancestors = _closure(roots, successors, ancestor_candidates)
    descendants = _closure(new_keys, successors)
    kept_keys = rooted_ancestors | descendants

    copied_nodes = {}
    for key in sorted(kept_keys):
        source = new_graph.nodes[key]
        copied = ReachableObject(source.type, source.name)
        output.add_node(copied)
        copied_nodes[key] = copied

    for predecessor_key in sorted(kept_keys):
        for successor_key in sorted(successors[predecessor_key] & kept_keys):
            output.add_edge(copied_nodes[successor_key], copied_nodes[predecessor_key])

    marker = ReachableObject(ReachableObjectType.ANNO, NEW_NODE_MARKER)
    output.add_node(marker)
    for key in sorted(new_keys):
        output.add_edge(marker, copied_nodes[key])
    return output
