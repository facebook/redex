#! /usr/bin/env python3
# @lint-avoid-python-3-compatibility-imports

import argparse
import fileinput
import re

from collections import namedtuple

Position = namedtuple('Position', ['line', 'parent'])

def read_map(fn):
    """
    Reads a file with lines in the following format:
    Filename:line|parent_index

    The parent_index is the line in the map file at which the parent can be
    found. Assuming redex is working properly there will be no cycles in the
    parent pointers; i.e. the file should describe a forest.
    """
    line_map = []
    with open(fn) as f:
        for s in f:
            line, _sep, parent = s.partition('|')
            line_map.append(Position(line, int(parent) - 1))
    return line_map

def get_stack(line_map, idx):
    """
    Given a starting index, traverse the parent pointers until we reach the top
    of the tree.
    """
    positions = []
    while True:
        pos = line_map[idx]
        positions.append(pos.line)
        idx = pos.parent
        if idx < 0:
            break
    return positions

def process(line_map, line):
    if not line.startswith('\tat'):
        return line

    def remap(match):
        method = match.group(1)
        line = int(match.group(2))
        real_lines = get_stack(line_map, line - 1)
        return "\n".join(
                '%s(%s)' % (method, real_line) for real_line in real_lines)

    return re.sub('(.*)\(:(\d+)\)', remap, line)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('line_map')
    args = parser.parse_args()
    line_map = read_map(args.line_map)
    for line in fileinput.input('-'):
        print(process(line_map, line), end='')
