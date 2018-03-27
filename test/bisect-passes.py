#!/usr/bin/env python

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
import json
import subprocess


SPECIAL_PASSES = [
    'ReBindRefsPass',
    'InterDexPass',
]


def filter_special(passes):
    return [p for p in passes if p not in SPECIAL_PASSES]


def count_nonspecial_passes(passes):
    return sum(1 for p in filter_special(passes))


def slice_passes(passes, low, high):
    newpasses = []
    newidx = 0
    for oldidx in range(0, len(passes)):
        p = passes[oldidx]
        if p in SPECIAL_PASSES:
            newpasses.append(p)
            continue
        if newidx >= low and newidx < high:
            newpasses.append(p)
        newidx += 1
    return newpasses


def bisect(passes, config, config_path, cmd):
    l = 0
    u = count_nonspecial_passes(passes)
    while l < u - 1:
        m = (l + u) / 2
        testpasses = slice_passes(passes, l, m)
        print('Testing passes: ' + str(testpasses))
        config['redex']['passes'] = testpasses
        with open(config_path, 'w') as config_file:
            json.dump(config, config_file)
        ret = subprocess.call(cmd, shell=True)
        if ret == 0:
            l = m
        else:
            u = m
    return filter_special(slice_passes(passes, l, u))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmd', required=True, help='Command to run')
    parser.add_argument('--config', required=True, help='Config file to bisect')
    args = parser.parse_args()

    with open(args.config, 'r') as config_file:
        contents = config_file.read()
    config = json.loads(contents)

    try:
        bad_passes = bisect(config['redex']['passes'], config, args.config, args.cmd)
        print('FAILING PASSES:' + str(bad_passes))
    finally:
        with open(args.config, 'w') as config_file:
            config_file.write(contents)
