#!/usr/bin/env python3

# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import argparse
import re
import subprocess

from collections import defaultdict
from os.path import abspath, basename, dirname, getsize, isdir, isfile, join, \
        realpath, split


class NodeBase:
    pass


class NodeClass(NodeBase):
    def __init__(self, class_name):
        # Assumes this is the deobfuscated name.
        self.class_name = class_name

    def __str__(self):
        return "[CLASS]\\n" + self.class_name


class NodeMethod(NodeBase):
    def __init__(self, class_name, method_name, args, return_type):
        self.class_name = class_name
        self.method_name = method_name
        self.args = args
        self.return_type = return_type

    def __str__(self):
        return "[METHOD]\\n" + self.class_name + "." + self.method_name + ":\\n(" + self.args + ")" + self.return_type


class NodeField(NodeBase):
    def __init__(self, class_name, field_name, field_type):
        self.class_name = class_name
        self.field_name = field_name
        self.field_type = field_type

    def __str__(self):
        return "[FIELD]\\n" + self.class_name + "." + self.field_name + ":\\n" + self.field_type


class NodeSeed(NodeBase):
    def __init__(
        self, class_name, bytype, bystring, computed, seed, keep,
        includedescriptorclasses, allowshrinking, allowoptimization,
        allowobfuscation, assumenosideeffects, blanket_keep, whyareyoukeeping,
        keep_count
    ):
        self.class_name = class_name
        self.bytype = bytype
        self.bystring = bystring
        self.computed = computed
        self.seed = seed
        self.keep = keep
        self.includedescriptorclasses = includedescriptorclasses
        self.allowshrinking = allowshrinking
        self.allowoptimization = allowoptimization
        self.allowobfuscation = allowobfuscation
        self.assumenosideeffects = assumenosideeffects
        self.blanket_keep = blanket_keep
        self.whyareyoukeeping = whyareyoukeeping
        self.keep_count = keep_count

    def __str__(self):
        return "[SEED]\\n" + self.class_name + ":\\n" + str(self.bytype) + str(
            self.bystring
        ) + str(self.computed) + str(self.seed) + str(self.keep) + str(
            self.includedescriptorclasses
        ) + str(self.allowshrinking) + str(self.allowoptimization) + str(
            self.allowobfuscation
        ) + str(self.assumenosideeffects) + str(self.blanket_keep) + str(
            self.whyareyoukeeping
        ) + " " + str(self.keep_count)


class NodeAnno(NodeBase):
    def __init__(self, type, visibility, annotations):
        self.type = type
        self.visibility = visibility
        self.annotations = annotations

    def __str__(self):
        return "[ANNO]\\ntype:{self.type}\\nvisibility:{self.visibility}\\nannotations:{self.annotations}".format(
            self=self
        )


class Digraph:
    """ An extremely simple directional graph """

    def __init__(self):
        self._nodes = set()
        self._outgoing_edges = defaultdict(set)
        self._incoming_edges = defaultdict(set)

    def add_node(self, u):
        self._nodes.add(u)

    def add_edge(self, u, v):
        """ Add edge: u -> v """
        self.add_node(u)
        self.add_node(v)
        self._outgoing_edges[u].add(v)
        self._incoming_edges[v].add(u)

    def get_roots(self):
        """ Return nodes with no incoming edges """
        return set(
            filter(lambda n: len(self._incoming_edges[n]) == 0, self._nodes)
        )

    def get_all_paths_from(self, root):
        """ Return all paths from root """
        # Assumes no cycle
        if len(self._outgoing_edges[root]) == 0:
            return [[root]]

        result = []
        for v in self._outgoing_edges[root]:
            result += [[root] + path for path in self.get_all_paths_from(v)]
        return result


# Very very rough method component parsing
# class_name;.method_name:(args)return_type
method_name_template = re.compile(
    "(L[_a-zA-Z0-9/$]+;)\.([<>_a-zA-Z0-9$]+):\(([\[_a-zA-Z0-9/$;]*)\)([\[]*[_a-zA-Z0-9/$;]+)"
)

# class_name;.field_name:field_type
field_name_template = re.compile(
    "(L[_a-zA-Z0-9/$]+;)\.([_a-zA-Z0-9$]+):([\[]*[_a-zA-Z0-9/$;]+)"
)

# class_name; [01]{12} \d+
seed_template = re.compile(
    "(L[_a-zA-Z0-9/$]+;) ([01])([01])([01])([01])([01])([01])([01])([01])([01])([01])([01])([01]) ([0-9]+)"
)

anno_template = re.compile("type:(.*) visibility:(.*) annotations:(.*)")


def parse_node(s):
    if not s.startswith('"') or not s.endswith('"'):
        raise ValueError("node is not enclosed by double quotes")
    s = s[1:-1]
    if s.startswith("[CLASS] "):
        return NodeClass(s[len("[CLASS] "):])
    elif s.startswith("[METHOD] "):
        m = method_name_template.fullmatch(s[len("[METHOD] "):])
        if m is None or len(m.groups()) != 4:
            raise ValueError("wrong method name: " + s)
        return NodeMethod(*m.groups())
    elif s.startswith("[FIELD] "):
        m = field_name_template.fullmatch(s[len("[FIELD] "):])
        if m is None or len(m.groups()) != 3:
            raise ValueError("wrong field name: " + s)
        return NodeField(*m.groups())
    elif s.startswith("[SEED] "):
        m = seed_template.fullmatch(s[len("[SEED] "):])
        if m is None or len(m.groups()) != 14:
            raise ValueError("wrong seed: " + s)
        return NodeSeed(m[1], *[int(e) for e in m.groups()[1:]])
    elif s.startswith("[ANNO] "):
        m = anno_template.fullmatch(s[len("[ANNO] "):])
        if m is None or len(m.groups()) != 3:
            raise ValueError("wrong anno: " + s)
        return NodeAnno(*m.groups())
    else:
        raise ValueError("unknown type: " + s)


def load_whitelist(file_name):
    if not file_name:
        return {}

    with open(file_name) as f:
        return {line.strip() for line in f.readlines()}


def build_graph(file_name, whitelist):
    graph = Digraph()
    with open(file_name) as f:
        for line in f:
            # line consists of: "class_name\tnodeA\tnodeB\t...\tnodeN", where
            # nodeA -> nodeB -> ... -> nodeN, and "nodeB is reachable via nodeA".
            tokens = line.strip().split('\t')
            cls_name = tokens[0]
            if cls_name in whitelist:
                nodes = [parse_node(token) for token in tokens[1:]]
                if len(nodes) < 2:
                    print("Orphan reachable node: " + str(nodes[0]))
                    graph.add_node(nodes[0])
                else:
                    for u_index, v in enumerate(nodes[1:], 1):
                        graph.add_edge(nodes[u_index - 1], v)
    return graph


def generate_dot(graph, out_dot, out_pdf):
    with open(out_dot, "w") as f:
        f.write("digraph g {\n")
        for root in graph.get_roots():
            for path in graph.get_all_paths_from(root):
                f.write(" -> ".join('"' + str(n) + '"' for n in path) + ";\n")
        f.write("}\n")
    subprocess.call(["dot", "-Tpdf", out_dot, "-o", out_pdf])


def test_graph():
    graph = Digraph()
    graph.add_edge("a", "b")
    graph.add_edge("a", "c")
    graph.add_edge("b", "d")
    graph.add_edge("d", "e")
    graph.add_edge("d", "f")
    graph.add_edge("z", "x")
    # Should print out:
    # [['a', 'c'], ['a', 'b', 'd', 'e'], ['a', 'b', 'd', 'f']]
    # [['z', 'x']]
    for root in graph.get_roots():
        print(graph.get_all_paths_from(root))


def main(args):
    graph = build_graph(args.input_graph, load_whitelist(args.filter))
    generate_dot(graph, args.out_dot, args.out)
    return


def parse_args():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=""
    )

    parser.add_argument(
        'input_graph',
        help='Input graph dump file from ReachabilityGraphPrinterPass'
    )
    parser.add_argument(
        '-f',
        '--filter',
        nargs='?',
        default='',
        help='File name containing classes of interests (whitelist)'
    )
    parser.add_argument(
        '-o', '--out', nargs='?', default='out.pdf', help='Resulting PDF file'
    )
    parser.add_argument(
        '-d',
        '--out-dot',
        nargs='?',
        default='out.dot',
        help='Resulting DOT file'
    )

    return parser.parse_args()


if __name__ == '__main__':
    main(parse_args())
