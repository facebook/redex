# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import os
import sys

def want_trace():
    try:
        trace = os.environ['TRACE']
    except KeyError:
        return False
    for t in trace.split(','):
        try:
            return int(t) > 0
        except ValueError:
            pass
        try:
            (module, level) = t.split(':')
            if module == 'REDEX' and int(level) > 0:
                return True
        except ValueError:
            pass
    return False


def log(*stuff):
    if want_trace():
        print(*stuff, file=sys.stderr)
