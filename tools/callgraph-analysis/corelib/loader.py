#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import array
import mmap
import struct


class CallGraphNode(object):
    def __init__(self, name, location):
        self.name = name
        self.location = location
        self.preds = set()
        self.succs = set()

    def __str__(self):
        return "%s{%s}\n" % (self.name, self.location)


class CallGraph(object):
    def __init__(self):
        self.nodes = {}

    def read_node(self, mapping):
        node_name_size = struct.unpack("<L", mapping.read(4))[0]
        node_name = mapping.read(node_name_size).decode("ascii")
        split_name = node_name.split("{")
        assert len(split_name) == 2
        return CallGraphNode(split_name[0], split_name[1][:-1])

    def add_node(self, node):
        self.nodes[node.name] = node

    def add_edge(self, node, succ):
        if succ not in node.succs:
            node.succs.add(succ)

        if node not in succ.preds:
            succ.preds.add(node)

    def get_node(self, node_name):
        return self.nodes[node_name]

    def get_node_preds(self, node_name):
        return self.nodes[node_name].preds

    def get_node_succs(self, node_name):
        return self.nodes[node_name].succs

    def read_header(self, mapping):
        magic = struct.unpack("<L", mapping.read(4))[0]
        if magic != 0xFACEB000:
            raise Exception("Magic number mismatch")
        version = struct.unpack("<L", mapping.read(4))[0]
        if version != self.expected_version():
            raise Exception("Version mismatch")

    def expected_version(self):
        return 1

    def load(self, fn):
        with open(fn) as f:
            mapping = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            self.read_header(mapping)
            nodes_count = struct.unpack("<L", mapping.read(4))[0]
            nodes = [None] * nodes_count
            out_edges = [None] * nodes_count
            print(nodes_count)
            for i in range(nodes_count):
                node = self.read_node(mapping)
                nodes[i] = node
                self.add_node(node)
                edges_size = struct.unpack("<L", mapping.read(4))[0]
                out_edges[i] = array.array("I")
                out_edges[i].frombytes(mapping.read(4 * edges_size))
            for i in range(nodes_count):
                node = nodes[i]
                for succ in out_edges[i]:
                    succ_node = nodes[succ]
                    self.add_edge(node, succ_node)
