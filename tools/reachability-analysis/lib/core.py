#!/usr/bin/env python3

# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import array
import mmap
import os
import shutil
import struct
import subprocess
import tempfile


class ReachableObjectType(object):
    ANNO = 0
    CLASS = 1
    FIELD = 2
    METHOD = 3
    SEED = 4

    @staticmethod
    def to_string(v):
        if v == ReachableObjectType.ANNO:
            return "ANNO"
        if v == ReachableObjectType.CLASS:
            return "CLASS"
        if v == ReachableObjectType.FIELD:
            return "FIELD"
        if v == ReachableObjectType.METHOD:
            return "METHOD"
        if v == ReachableObjectType.SEED:
            return "SEED"


# Aside from classes and annotations, the other nodes will never have collisions
# in their node names. Thus, we are able to infer their node type just by
# looking at their names. The functions below help with that.


def is_method(node_name):
    return "(" in node_name


def is_field(node_name):
    return ":" in node_name and not is_method(node_name)


def is_seed(node_name):
    return node_name == "<SEED>"


def show_list_with_idx(list):
    ret = ""
    i = 0
    while i < len(list):
        ret += "%d: %s\n" % (i, list[i])
        i += 1

    return ret


def download_from_everstore(handle, filename):
    subprocess.check_call(["clowder", "get", handle, filename])


class ReachableObject(object):
    def __init__(self, type, name):
        self.type = type
        self.name = name
        self.preds = []
        self.succs = []

    def __str__(self):
        return self.name

    def __repr__(self):
        ret = "%s: %s\n" % (ReachableObjectType.to_string(self.type), self.name)
        ret += "Reachable from %d predecessor(s):\n" % len(self.preds)
        ret += show_list_with_idx(self.preds)
        ret += "Reaching %d successor(s):\n" % len(self.succs)
        ret += show_list_with_idx(self.succs)
        return ret


class ReachableMethod(ReachableObject):
    # we need override info for a method
    def __init__(self, ro, mog):
        self.type = ro.type
        self.name = ro.name
        self.preds = ro.preds
        self.succs = ro.succs
        self.overriding = []
        self.overriden_by = []

        if self.name in mog.nodes.keys():
            n = mog.nodes[self.name]
            self.overriding = n.parents
            self.overriden_by = n.children

    def __repr__(self):
        ret = super(ReachableMethod, self).__repr__()
        if len(self.overriding) != 0:
            ret += "Overriding %s methods:\n" % len(self.overriding)
            ret += show_list_with_idx(
                list(map(lambda n: n.name, self.overriding)))

        if len(self.overriden_by) != 0:
            ret += "Overriden by %s methods:\n" % len(self.overriden_by)
            ret += show_list_with_idx(
                list(map(lambda n: n.name, self.overriden_by)))
        return ret


class AbstractGraph(object):
    """
    This contains the deserialization counterpart to the graph serialization
    code in BinarySerialization.h.
    """

    def __init__(self):
        self.nodes = {}

    def expected_version(self):
        raise NotImplementedError()

    def read_node(self, mapping):
        raise NotImplementedError()

    def add_node(self, node):
        raise NotImplementedError()

    def add_edge(self, n1, n2):
        raise NotImplementedError()

    def list_nodes(self, search_str=None):
        raise NotImplementedError()

    def read_header(self, mapping):
        magic = struct.unpack("<L", mapping.read(4))[0]
        if magic != 0xFACEB000:
            raise Exception("Magic number mismatch")
        version = struct.unpack("<L", mapping.read(4))[0]
        if version != self.expected_version():
            raise Exception("Version mismatch")

    def load(self, fn):
        with open(fn) as f:
            mapping = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            self.read_header(mapping)
            nodes_count = struct.unpack("<L", mapping.read(4))[0]
            nodes = [None] * nodes_count
            out_edges = [None] * nodes_count
            for i in range(nodes_count):
                node = self.read_node(mapping)
                nodes[i] = node
                self.add_node(node)

                edges_size = struct.unpack("<L", mapping.read(4))[0]
                out_edges[i] = array.array("I")
                out_edges[i].frombytes(mapping.read(4 * edges_size))

            for i in range(nodes_count):
                node = nodes[i]
                for target in out_edges[i]:
                    target_node = nodes[target]
                    self.add_edge(node, target_node)


class ReachabilityGraph(AbstractGraph):
    @staticmethod
    def expected_version():
        return 1

    def read_node(self, mapping):
        node_type = struct.unpack("<B", mapping.read(1))[0]
        node_name_size = struct.unpack("<L", mapping.read(4))[0]
        node_name = mapping.read(node_name_size).decode("ascii")
        return ReachableObject(node_type, node_name)

    def add_node(self, node):
        self.nodes[(node.type, node.name)] = node

    def list_nodes(self, search_str=None):
        for key in self.nodes.keys():
            type = ReachableObjectType.to_string(key[0])
            name = key[1]
            if search_str is None or search_str in name:
                print("(ReachableObjectType.%s, \"%s\")" % (type, name))

    @staticmethod
    def add_edge(n1, n2):
        if n1 not in n2.succs:
            n2.succs.append(n1)

        if n2 not in n1.preds:
            n1.preds.append(n2)

    def get_node(self, node_name):
        if is_method(node_name):
            return self.nodes[(ReachableObjectType.METHOD, node_name)]
        if is_field(node_name):
            return self.nodes[(ReachableObjectType.FIELD, node_name)]
        # If we get here, we may have an annotation or a class. Just assume
        # we have a class. Users should call `get_anno` if they want to
        # retrieve an annotation.
        return self.nodes[(ReachableObjectType.CLASS, node_name)]

    def get_anno(self, node_name):
        return self.nodes[(ReachableObjectType.ANNO, node_name)]

    def get_seed(self, node_name):
        return self.nodes[(ReachableObjectType.SEED, node_name)]


class MethodOverrideGraph(AbstractGraph):
    class Node(object):
        def __init__(self, name):
            self.name = name
            self.parents = []
            self.children = []

    def __init__(self):
        self.nodes = {}

    @staticmethod
    def expected_version():
        return 1

    def read_node(self, mapping):
        node_name_size = struct.unpack("<L", mapping.read(4))[0]
        node_name = mapping.read(node_name_size).decode("ascii")
        return self.Node(node_name)

    def add_node(self, node):
        self.nodes[node.name] = node

    def list_nodes(self, search_str=None):
        for key in self.nodes.keys():
            if search_str is None or search_str in key:
                print("\"%s\"" % key)

    @staticmethod
    def add_edge(method, child):
        method.children.append(child)
        child.parents.append(method)


class CombinedGraph(object):
    def __init__(self, reachability, method_override):
        self.reachability_graph = ReachabilityGraph()
        self.reachability_graph.load(reachability)
        self.method_override_graph = MethodOverrideGraph()
        self.method_override_graph.load(method_override)

        # extract information from the override graph
        for (type, name) in self.reachability_graph.nodes:
            if type == ReachableObjectType.METHOD:
                self.reachability_graph.nodes[(type, name)] = ReachableMethod(
                    self.reachability_graph.nodes[(type, name)],
                    self.method_override_graph)

        for method in self.method_override_graph.nodes.keys():
            method_node = self.reachability_graph.get_node(method)
            for child in method_node.overriden_by:
                # find child in reachability graph, then build edge
                method_child = self.reachability_graph.get_node(child.name)
                for pred in method_node.preds:
                    if pred.type == ReachableObjectType.METHOD:
                        self.reachability_graph.add_edge(method_child, pred)

        self.nodes = self.reachability_graph.nodes

    @staticmethod
    def from_everstore(reachability, method_override):
        temp_dir = tempfile.mkdtemp()
        r_tmp = os.path.join(temp_dir, "redex-reachability.graph")
        download_from_everstore(reachability, r_tmp)
        mog_tmp = os.path.join(temp_dir, "redex-method-override.graph")
        download_from_everstore(method_override, mog_tmp)
        ret = CombinedGraph(r_tmp, mog_tmp)
        shutil.rmtree(temp_dir)
        return ret

    def node(self, search_str=None, search_type=None):
        node = None
        known_names = []
        for (type, name) in self.nodes.keys():
            if search_type is not None and type != search_type:
                # Classes and Annotations may have naming collisions
                # if that happens, use the search_type argument to filter
                continue
            if search_str is None or search_str in name:
                known_names += [(type, name)]

        if search_str is not None and len(known_names) == 1:
            # know exactly one
            node = self.nodes[known_names[0]]
        elif search_str is not None:
            # there could be names containing name of another node
            # in this case we prefer the only exact match
            exact_match = list(filter(
                (lambda n: n[1] == search_str), known_names))
            if len(exact_match) == 1:
                node = self.nodes[exact_match[0]]

        # if after all we still can't get which one does the user want,
        # print all options
        if node is None:
            print("Found %s matching names:" % len(known_names))
            idx = 0
            for (type, name) in known_names:
                print("%d: (ReachableObjectType.%s, \"%s\")"
                    % (idx, ReachableObjectType.to_string(type), name))
                idx += 1

            return lambda i: self.nodes[known_names[i]]

        return node
