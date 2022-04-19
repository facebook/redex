#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import json
import subprocess


SPECIAL_PASSES = ["ReBindRefsPass", "InterDexPass"]


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
    lower = 0
    upper = count_nonspecial_passes(passes)
    while lower < upper - 1:
        m = (lower + upper) / 2
        testpasses = slice_passes(passes, lower, m)
        print(("Testing passes: " + str(testpasses)))
        config["redex"]["passes"] = testpasses
        with open(config_path, "w") as config_file:
            json.dump(config, config_file)
        ret = subprocess.call(cmd, shell=True)  # noqa: P204
        if ret == 0:
            lower = m
        else:
            upper = m
    return filter_special(slice_passes(passes, lower, upper))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmd", required=True, help="Command to run")
    parser.add_argument("--config", required=True, help="Config file to bisect")
    args = parser.parse_args()

    with open(args.config, "r") as config_file:
        contents = config_file.read()
    config = json.loads(contents)

    try:
        bad_passes = bisect(config["redex"]["passes"], config, args.config, args.cmd)
        print(("FAILING PASSES:" + str(bad_passes)))
    finally:
        with open(args.config, "w") as config_file:
            config_file.write(contents)
