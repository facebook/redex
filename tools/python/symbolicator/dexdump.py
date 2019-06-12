# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
import re

class DexdumpSymbolicator(object):

    CLASS_REGEX = re.compile(
        r'\bL(?P<class>[A-Za-z][0-9A-Za-z_$]*\/[0-9A-Za-z_$\/]+);')

    LINE_REGEX = re.compile(r'(?P<prefix>0x[0-9a-f]+ line=)(?P<lineno>\d+)')

    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps

    def class_replacer(self, matchobj):
        m = matchobj.group('class')
        cls = m.replace('/', '.')
        if cls in self.symbol_maps.class_map:
            return 'L%s;' % self.symbol_maps.class_map[cls].replace('.', '/')
        return 'L%s;' % m

    def line_replacer(self, matchobj):
        lineno = int(matchobj.group('lineno'))
        positions = map(
                lambda p: '%s:%d' % (p.file, p.line),
                self.symbol_maps.line_map.get_stack(lineno - 1))
        return matchobj.group('prefix') + ', '.join(positions)

    def symbolicate(self, line):
        line = self.CLASS_REGEX.sub(self.class_replacer, line)
        line = self.LINE_REGEX.sub(self.line_replacer, line)
        return line

    @staticmethod
    def is_likely_dexdump(line):
        return re.match(r"^Processing '.*\.dex'", line) or\
                re.search(r'Class #\d+', line)
