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


class Graph(object):
    def __init__(self):
        self.nodes = {}

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

    def add_node(self, node):
        self.nodes[(node.type, node.name)] = node

    @staticmethod
    def add_edge(n1, n2):
        n1.succs.append(n2)
        n2.preds.append(n1)

    @staticmethod
    def load(fn):
        with open(fn) as f:
            mapping = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            magic = struct.unpack("<L", mapping.read(4))[0]
            if magic != 0xFACEB000:
                raise Exception("Magic number mismatch")
            version = struct.unpack("<L", mapping.read(4))[0]
            if version != 1:
                raise Exception("Version mismatch")

            nodes_count = struct.unpack("<L", mapping.read(4))[0]
            nodes = [None] * nodes_count
            preds = [None] * nodes_count
            graph = Graph()
            for i in range(nodes_count):
                node_type = struct.unpack("<B", mapping.read(1))[0]
                node_name_size = struct.unpack("<L", mapping.read(4))[0]
                node_name = mapping.read(node_name_size).decode("ascii")

                preds_size = struct.unpack("<L", mapping.read(4))[0]
                preds[i] = array.array("I")
                preds[i].frombytes(mapping.read(4 * preds_size))

                node = ReachableObject(node_type, node_name)
                nodes[i] = node
                graph.add_node(node)

            for i in range(0, nodes_count):
                succ_node = nodes[i]
                for pred in preds[i]:
                    pred_node = nodes[pred]
                    graph.add_edge(pred_node, succ_node)

            return graph
