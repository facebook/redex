#!/usr/bin/env python3

# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from collections import defaultdict

from . import core
from .core import ReachableObjectType


"""
Just a kitchen sink of methods that may be useful when analyzing the
reachability graph. We may want to refine / prune this file in the future.
"""


def classify_nodes(nodes):
    """
    Return a summary count of the number of each kind of node.
    """
    ctr = defaultdict(int)
    for node in nodes:
        ctr[ReachableObjectType.to_string(node.type)] += 1
    return ctr


def group_by_common_keys(d):
    """
    The input :d should be a dict whose values are sets. Suppose we had

      k1 -> set(v1, v2, v3)
      k2 -> set(v1, v2)
      k3 -> set(v1, v2)

    The output will be

      frozenset(k1, k2, k3) -> set(v1, v2)
      frozenset(k1) -> set(v3)

    This can be useful for finding keep rules which overlap a lot in their
    matched classes.
    """
    value_to_keys = defaultdict(set)
    for k, values in d.items():
        for v in values:
            value_to_keys[v].add(k)

    grouped_keys = defaultdict(set)
    for value, keys in value_to_keys.items():
        grouped_keys[frozenset(keys)].add(value)

    return grouped_keys


def find_nodes(graph, filter_fn):
    """
    Find all nodes whose names pass the predicate :filter_fn.
    """
    nodes = set()
    for node in graph.nodes.values():
        if filter_fn(node.name):
            nodes.add(node)
    return nodes


def find_nodes_in_packages(graph, pkg_prefixes):
    """
    Find all nodes that fall under the list of :pkg_prefixes.
    """
    nodes = set()
    for node in graph.nodes.values():
        for pkg_prefix in pkg_prefixes:
            # If we have an array, use its base type
            base_type = node.name.lstrip("[")
            if base_type.startswith(pkg_prefix):
                nodes.add(node)
    return nodes


def find_boundary(graph, query_set):
    """
    Find all the nodes that retain (i.e. point into) :query_set.

    If we encounter an annotation node, we look for its retainers. Frequently,
    they are members of the set itself. E.g. we have many instances of

        @JsonSerialize(Foo.class)
        public Foo { ... }

    We don't want to mark JsonSerialize as a retainer of Foo.

    Return a dictionary of

      <retaining node> -> <set of retained nodes>

    where the set of retained nodes are the immediate successors of the
    retaining node that are inside :query_set.
    """
    boundary = defaultdict(set)
    to_visit = set(query_set)
    visited = set()
    while len(to_visit) > 0:
        node = to_visit.pop()
        if node in visited:
            continue
        visited.add(node)

        for pred in node.preds:
            if pred not in query_set:
                if not pred.type == ReachableObjectType.ANNO:
                    boundary[pred].add(node)
                else:
                    to_visit.add(pred)
    return boundary


def group_members_by_class(graph):
    """
    Return a map of class -> set of members in that class.
    """
    grouped_members = defaultdict(set)
    for (ty, name), node in graph.nodes.items():
        if ty in [ReachableObjectType.FIELD, ReachableObjectType.METHOD]:
            cls, sep, _ = name.partition(";")
            cls += ";"
            try:
                cls_node = graph.get_node(cls)
            except KeyError:
                continue
            grouped_members[cls_node].add(node)
    return grouped_members


def find_package_references(graph, pkg_prefixes):
    """
    Find all nodes that retain classes under the list of :pkg_prefixes.
    """
    query_set = find_nodes_in_packages(graph, pkg_prefixes)
    return find_boundary(graph, query_set)


def get_dominated(graph, query_set):
    """
    Find all nodes in the graph that cannot be reached from a root without
    passing through :query_set.
    """
    visited = set()

    def mark(node):
        if node in visited:
            return
        if node in query_set:
            return
        visited.add(node)
        for succ in node.succs:
            mark(succ)

    seeds = [node for node in graph.nodes.values() if len(node.preds) == 0]

    for seed in seeds:
        mark(seed)

    closure = set()
    for node in graph.nodes.values():
        if node not in visited:
            closure.add(node)

    return closure


class Ranker(object):
    """
    Given a list of leak roots and leaked methods, rank the roots in order of
    classes that they retain. We do this by making a weighted count of the
    number of nodes reachable from each root. Methods are weighted most heavily
    and fields least. For a given node reachable from N roots, we divide its
    weight by N when calculating its contribution to each root's score. This is
    similar to the LeakShare metric described in "BLeak: Automatically Debugging
    Memory Leaks in Web Applications" by Vilk and Berger.

    While calculating the ranking, we also record the set of leaked nodes
    dominated by each root, i.e. the set of nodes that are only reachable via
    that root.
    """

    def __init__(self, roots, leaked_set):
        self.counts = defaultdict(int)
        self.leaked_set = leaked_set
        for root in roots:
            self.mark(root)

    def mark(self, root):
        """
        Populate self.counts so that it contains, for each leaked node, the
        number of leak roots that it can be reached from.
        """
        visited = set()

        def mark_rec(node):
            if node in visited or node not in self.leaked_set:
                return
            visited.add(node)
            self.counts[node] += 1
            for succ in node.succs:
                mark_rec(succ)

        for succ in root.succs:
            mark_rec(succ)

    def get_rank(self, root):
        rank = 0.0
        dominated = set()
        visited = set()

        def visit(node):
            nonlocal rank
            nonlocal dominated

            if node in visited or node not in self.leaked_set:
                return
            visited.add(node)

            if self.counts[node] == 1:
                dominated.add(node)
            if node.type == ReachableObjectType.METHOD:
                rank += 1.0 / self.counts[node]
            for succ in node.succs:
                visit(succ)

        for succ in root.succs:
            visit(succ)

        return (rank, dominated)


def rank(roots, leaked_set):
    ranker = Ranker(roots, leaked_set)
    results = [(root, ranker.get_rank(root)) for root in roots]
    return sorted(results, key=lambda t: t[1][0], reverse=True)
