#!/usr/bin/env python3

# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import array
import mmap
import struct


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


class ReachableObject(object):
    def __init__(self, type, name):
        self.type = type
        self.name = name
        self.preds = []
        self.succs = []

    def __str__(self):
        return self.name

    def __repr__(self):
        return "<RO %s>" % self.name


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
        n2.succs.append(n1)
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
