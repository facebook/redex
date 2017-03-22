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

import os
import sys

trace = None

ALL = '__ALL__'

def parse_trace_string(trace):
    """
    The trace string is of the form KEY1:VALUE1,KEY2:VALUE2,...

    We convert it into a dict here.
    """
    rv = {}
    for t in trace.split(','):
        try:
            module, level = t.split(':')
            rv[module] = int(level)
        except ValueError:
            rv[ALL] = int(t)
    return rv


def get_trace():
    global trace
    if trace is not None:
        return trace
    if 'TRACE' in os.environ:
        trace = parse_trace_string(os.environ['TRACE'])
    else:
        trace = {}
    return trace


def get_log_level():
    trace = get_trace()
    return max(trace.get('REDEX', 0), trace.get(ALL, 0))


def strip_trace_tag(env):
    """
    Remove the "REDEX:N" component from the trace string
    """
    env = env.copy()
    try:
        trace = parse_trace_string(env['TRACE'])
        trace.pop('REDEX')
        if ALL in trace:
            trace_str = str(trace[ALL])
            trace.pop(ALL)
        else:
            trace_str = ''
        trace_str += ','.join(k + ':' + v for k, v in trace.iteritems())
        env['TRACE'] = trace_str
        return env
    except KeyError:
        return env


def log(*stuff):
    if get_log_level() > 0:
        print(*stuff, file=sys.stderr)
