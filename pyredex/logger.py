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

def parse_trace_string(trace):
    """
    The trace string is of the form KEY1:VALUE1,KEY2:VALUE2,...

    We convert it into a dict here.
    """
    rv = {}
    for t in trace.split(','):
        (module, level) = t.split(':')
        rv[module] = int(level)
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
    return get_trace().get('REDEX', 0)

def strip_trace_tag(env):
    """
    Remove the "REDEX:N" component from the trace string
    """
    env = env.copy()
    try:
        trace_str = env['TRACE']
        trace = parse_trace_string(trace_str)
        trace.pop('REDEX')
        trace_str = ','.join(k + ':' + v for k, v in trace.iteritems())
        env['TRACE'] = trace_str
        return env
    except KeyError:
        return env

def log(*stuff):
    if get_log_level() > 0:
        print(*stuff, file=sys.stderr)
