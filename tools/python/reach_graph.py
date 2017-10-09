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
import collections

from collections import defaultdict
from os.path import abspath, basename, dirname, getsize, isdir, isfile, join, \
        realpath, split


def flatten(l):
    for el in l:
        if isinstance(el, collections.Iterable
                     ) and not isinstance(el, (str, bytes)):
            yield from flatten(el)
        else:
            yield el


class NodeBase:
    def __hash__(self):
        return hash(str(self))

    def get_class_name(self):
        if hasattr(self, "class_name"):
            return self.class_name
        else:
            return ""


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
        args = self.args.replace(";", ";\\n")
        return "[METHOD]\\n" + self.class_name + "\\n" + self.method_name + "\\n" + args + "Return: " + self.return_type


class NodeField(NodeBase):
    def __init__(self, class_name, field_name, field_type):
        self.class_name = class_name
        self.field_name = field_name
        self.field_type = field_type

    def __str__(self):
        return "[FIELD]\\n" + self.class_name + "\\n." + self.field_name + ":\\n" + self.field_type


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
        ) + "_" + str(self.keep_count)


class NodeAnno(NodeBase):
    def __init__(self, type, visibility, annotations):
        self.type = type
        self.visibility = visibility
        self.annotations = annotations

    def __str__(self):
        annos = self.annotations.replace(";", ";\\n")
        return "[ANNO]\\ntype:{self.type}\\nvisibility:{self.visibility}\\nannotations:{annos}".format(
            self=self, annos=annos
        )


class NodeContainer(NodeBase):
    def __init__(self, nodes):
        assert (type(nodes) == list)
        self.nodes = nodes
        self.short_name = ""

    def __str__(self):
        return "\n".join(str(n) for n in self.nodes)

    def get_short_name(self):
        return self.short_name


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

    def remove_node(self, u):
        if u not in self._nodes:
            return
        if u in self._incoming_edges:
            for pred in self._incoming_edges[u]:
                self._outgoing_edges[pred].remove(u)
        if u in self._outgoing_edges:
            for succ in self._outgoing_edges[u]:
                self._incoming_edges[succ].remove(u)
        self._nodes.remove(u)

    def get_roots(self):
        """ Return nodes with no incoming edges """
        roots = filter(lambda n: len(self._incoming_edges[n]) == 0, self._nodes)
        return sorted(roots, key=lambda x: str(x))

    def get_all_paths_from(self, root):
        """ Return all paths from root """
        # Assumes no cycle
        if len(self._outgoing_edges[root]) == 0:
            return [[root]]

        result = []
        for v in self._outgoing_edges[root]:
            result += [[root] + path for path in self.get_all_paths_from(v)]
        return sorted(result, key=lambda x: str(x))


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

# For uniqueness
cached_nodes = {}


def create_or_get_node(s):
    if not s.startswith('"') or not s.endswith('"'):
        raise ValueError("node is not enclosed by double quotes")
    s = s[1:-1]
    if s in cached_nodes:
        return cached_nodes[s]

    if s.startswith("[CLASS] "):
        obj = NodeClass(s[len("[CLASS] "):])
    elif s.startswith("[METHOD] "):
        m = method_name_template.fullmatch(s[len("[METHOD] "):])
        if m is None or len(m.groups()) != 4:
            raise ValueError("wrong method name: " + s)
        obj = NodeMethod(*m.groups())
    elif s.startswith("[FIELD] "):
        m = field_name_template.fullmatch(s[len("[FIELD] "):])
        if m is None or len(m.groups()) != 3:
            raise ValueError("wrong field name: " + s)
        obj = NodeField(*m.groups())
    elif s.startswith("[SEED] "):
        m = seed_template.fullmatch(s[len("[SEED] "):])
        if m is None or len(m.groups()) != 14:
            raise ValueError("wrong seed: " + s)
        obj = NodeSeed(m[1], *[int(e) for e in m.groups()[1:]])
    elif s.startswith("[ANNO] "):
        m = anno_template.fullmatch(s[len("[ANNO] "):])
        if m is None or len(m.groups()) != 3:
            raise ValueError("wrong anno: " + s)
        obj = NodeAnno(*m.groups())
    else:
        raise ValueError("unknown type: " + s)

    cached_nodes[s] = obj
    return obj


def load_whitelist(file_name):
    if not file_name:
        return {}

    with open(file_name) as f:
        return {line.strip() for line in f.readlines()}


def build_graph(file_name, whitelist):
    graph = Digraph()
    remains = whitelist.copy()
    with open(file_name) as f:
        for line in f:
            # line consists of: "class_name\tnodeA\tnodeB\t...\tnodeN", where
            # nodeA -> nodeB -> ... -> nodeN, and "nodeB is reachable via nodeA".
            tokens = line.strip().split('\t')
            cls_name = tokens[0]
            if cls_name in whitelist:
                remains.remove(cls_name)
                nodes = [create_or_get_node(token) for token in tokens[1:]]
                if len(nodes) < 2:
                    print("Orphan reachable node: " + str(nodes[0]))
                    graph.add_node(nodes[0])
                else:
                    for u_index, v in enumerate(nodes[1:], 1):
                        graph.add_edge(nodes[u_index - 1], v)

    for e in sorted(remains):
        print("Warning: cannot find \"{}\"".format(e))

    # print(len(graph._nodes))
    # print(len({str(n) for n in graph._nodes}))
    # print(len({n for n in graph._nodes}))
    # print(len(graph._incoming_edges))
    # print(len(graph._outgoing_edges))
    return graph


force_kill = {
    "Ldalvik/annotation/EnclosingClass;", "Ldalvik/annotation/EnclosingMethod;",
    "Ldalvik/annotation/AnnotationDefault;", "Ldalvik/annotation/InnerClass;",
    "Ldalvik/annotation/MemberClasses;", "Ldalvik/annotation/Throws;"
}


def generate_dot(graph, out_dot, out_pdf, whitelist, color):
    def get_style(n):
        if n in root_set:
            return ", style=filled, fillcolor=gray"
        elif n.get_class_name() in color:
            return ", style=filled, fillcolor=blue"
        elif n.get_class_name() in whitelist:
            return ", style=filled, fillcolor=maroon"
        elif type(n) is NodeAnno and n.type in force_kill:
            return ", style=filled, fillcolor=red"
        else:
            return ""

    with open(out_dot, "w") as f:
        f.write("strict digraph g {\n")
        f.write("rankdir=LR\n")
        f.write("node [shape = box];\n")

        node_id = 0
        nodes = {}
        printed_nodes = set()
        cluster_id = 0

        # Writes all nodes
        roots = graph.get_roots()
        root_set = set(roots)
        for n in flatten(graph.get_all_paths_from(root) for root in roots):
            printed_nodes.add(n)
            if str(n) in nodes:
                continue

            # Seed nodes are in grey; Regressed nodes are in maroon.
            # System annotations are in red.
            style = ""

            node_id += 1
            nodes[str(n)] = "n" + str(node_id)

            if type(n) is not NodeContainer:
                f.write(
                    "n{} [label=\"{}\"{}]\n".
                    format(node_id, str(n), get_style(n))
                )
                continue

            # Create a subgraph for NodeContainer.
            # subgraph cluster1 {
            #     style=invis;
            # 		na -> nb -> nc [constraint=false];
            # 	}
            f.write("\n")
            for sub_id, sub_node in enumerate(n.nodes, 1):
                f.write(
                    "n{}_{} [label=\"{}\"{}]\n".
                    format(node_id, sub_id, str(sub_node), get_style(sub_node))
                )
            f.write("\n")
            cluster_id += 1
            f.write("subgraph cluster{} {{\n".format(cluster_id))
            f.write("  style=dotted;\n")
            f.write(
                "  " + " -> ".join(
                    "n" + str(node_id) + "_" + str(sub_id)
                    for sub_id in range(1, len(n.nodes) + 1)
                ) + " [constraint=false];\n"
            )
            f.write("}\n")

        # Writes all edges
        f.write("\n")
        for root in roots:
            for path in graph.get_all_paths_from(root):
                for u, v in zip(path[:-1], path[1:]):
                    f.write(
                        "{} -> {}; ".format(
                            nodes[str(u)] + (
                                "_" + str(len(u.nodes))
                                if type(u) is NodeContainer else ""
                            ),
                            nodes[str(v)] +
                            ("_1" if type(v) is NodeContainer else ""),
                        )
                    )
                f.write("\n")

        f.write("{\n")
        f.write("rank = same;\n")
        for root in graph.get_roots():
            if root not in printed_nodes:
                continue
            f.write(nodes[str(root)] + '\n')
        f.write("}\n")

        f.write("}\n")

    subprocess.call(["sed", "-i", "", "s#Lcom/facebook/#fb/#g", out_dot])
    subprocess.call(["dot", "-Tpdf", out_dot, "-o", out_pdf])


def compress_graph(graph, whitelist):
    to_compress = []

    def check_node(u):
        if type(u) is NodeSeed or type(u) is NodeAnno:
            return False
        if type(u) is NodeClass and u.class_name in whitelist and len(
            graph._outgoing_edges[u]
        ) == 0:
            return False
        return True

    compression = []
    visited = set()
    S = [root for root in graph.get_roots()]
    while len(S) != 0:
        u = S.pop()
        if u in visited:
            continue
        visited.add(u)
        for v in graph._outgoing_edges[u]:
            S.append(v)

        if len(graph._outgoing_edges[u]
              ) == 1 and check_node(u) and check_node(v):
            assert len(graph._outgoing_edges[u]) == 1
            assert len(graph._incoming_edges[v]) == 1
            if len(compression) == 0:
                compression.append(u)

            assert len(graph._outgoing_edges[compression[-1]]) == 1
            compression.append(v)
        else:
            if len(compression) != 0:
                to_compress.append(compression)
                compression = []

    for nodes in to_compress:
        compressed_node = NodeContainer(nodes)
        graph.add_node(compressed_node)
        for pred in graph._incoming_edges[nodes[0]]:
            graph.add_edge(pred, compressed_node)
        for succ in graph._outgoing_edges[nodes[-1]]:
            graph.add_edge(compressed_node, succ)
        for node in nodes:
            graph.remove_node(node)
    return


def test_graph():
    graph = Digraph()
    graph.add_edge("a", "b")
    graph.add_edge("a", "c")
    graph.add_edge("b", "d")
    graph.add_edge("d", "e")
    graph.add_edge("d", "f")
    graph.add_edge("z", "x")
    graph.add_edge("x", "x1")
    graph.add_edge("x", "x2")
    graph.add_edge("i", "j")
    print(
        list(
            flatten(
                graph.get_all_paths_from(root) for root in graph.get_roots()
            )
        )
    )
    # Should print out: (lexicographically sorted)
    # [['a', 'b', 'd', 'e'], ['a', 'b', 'd', 'f'], ['a', 'c']]
    # [['i', 'j']]
    # [['z', 'x', 'x1'], ['z', 'x', 'x2']]
    for root in graph.get_roots():
        print(graph.get_all_paths_from(root))


def main(args):
    wl = load_whitelist(args.filter)
    color = load_whitelist(args.color)
    graph = build_graph(args.input_graph, wl)
    compress_graph(graph, wl)
    generate_dot(graph, args.out_dot, args.out, wl, color)
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
        '--color',
        nargs='?',
        default='',
        help='File name containing classes to be colored differently'
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
