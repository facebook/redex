#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import array
import mmap
import re
import struct

re_methodname = r"^(ENTRY|EXIT|(L.+;\..+:\((\[?(L.+;|Z|B|S|C|I|J|F|D|V))*\)(\[?(L.+;|Z|B|S|C|I|J|F|D|V))))$"
re_classname = r"^L.+;$"


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
        self.node_classes = {}

    def read_node(self, mapping):
        node_name_size = struct.unpack("<L", mapping.read(4))[0]
        node_name = mapping.read(node_name_size).decode("ascii")
        split_name = node_name.split("{")
        assert len(split_name) == 2
        return CallGraphNode(split_name[0], split_name[1][:-1])

    def add_node(self, node):
        self.nodes[node.name] = node
        if ";" in node.name:
            split = node.name.split(".")
            class_name = split[0]
            method_name = split[1].split(":")[0]
            if class_name not in self.node_classes:
                self.node_classes[class_name] = {}
            if method_name not in self.node_classes[class_name]:
                self.node_classes[class_name][method_name] = []
            self.node_classes[class_name][method_name].append(node)

    def add_edge(self, node, succ):
        if succ not in node.succs:
            node.succs.add(succ)

        if node not in succ.preds:
            succ.preds.add(node)

    def get_node(self, node_name):
        if re.match(re_methodname, node_name) is None:
            print("input don't match method name regex")
            return None
        if node_name not in self.nodes:
            print("node don't exist in callgraph")
            return None
        return self.nodes[node_name]

    def get_nodes_in_class(self, class_name):
        if re.match(re_classname, class_name) is None:
            print("input don't match class name regex")
            return None
        if class_name not in self.node_classes:
            print("class not exist in callgraph")
            return None
        return_val = []
        for value in self.node_classes[class_name].values():
            return_val.extend(value)
        return return_val

    def get_nodes_in_class_method(self, class_name, method_name):
        if re.match(re_classname, class_name) is None:
            print("input don't match class name regex")
            return None
        if class_name not in self.node_classes:
            print("class not exist in callgraph")
            return None
        if method_name not in self.node_classes[class_name]:
            print("method not exist in class or callgraph")
            return None
        return self.node_classes[class_name][method_name]

    def get_node_preds(self, node_name):
        if re.match(re_methodname, node_name) is None:
            print("input don't match method name regex")
            return None
        if node_name not in self.nodes:
            print("node don't exist")
            return None
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
