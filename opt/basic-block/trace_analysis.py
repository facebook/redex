#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import argparse

import numpy as np


parser = argparse.ArgumentParser()

parser.add_argument(
    "-f",
    "--file",
    dest="trace_file",
    default="test_trace.csv",
    help="Trace file to read",
    metavar="FILE",
)
parser.add_argument(
    "-p",
    "--percentile",
    dest="percentile",
    default="99",
    help="The percentile values to print",
)
parser.add_argument(
    "-c",
    "--count",
    dest="countSize",
    default="1",
    help="Count number of methods of this size",
)


def main():
    args = parser.parse_args()
    num_methods = 0
    num_blocks = 0
    num_instructions = 0
    fan_in = 0
    block_size_array = []
    method_size_array = []
    num_virtual = 0
    trace = 0
    with open(args.trace_file, "r") as tr_file:
        for row in tr_file:
            all_stats = row.split(",")
            if all_stats[0] == "M" or all_stats[0] == "B":
                trace = 1
                if all_stats[0] == "M":
                    num_methods += 1
                    method_size_array.insert(len(method_size_array), int(all_stats[2]))
                    num_virtual += int(all_stats[4])
                elif all_stats[0] == "B":
                    num_blocks += 1
                    num_instructions += int(all_stats[2])
                    fan_in += int(all_stats[4])
                    block_size_array.insert(len(method_size_array), int(all_stats[2]))

    print("========Summary=========")
    if trace:
        print(("Num of Methods:  %d, Num of Blocks: %d" % (num_methods, num_blocks)))
        print(
            (
                "Blocks/Method: %.2f, Instructions/Block: %.2f"
                % (num_blocks / num_methods, num_instructions / num_blocks)
            )
        )
        print(("Average Degree: %.2f" % (fan_in / num_blocks)))
        print(("Number of Virtual Methods: %d" % (num_virtual)))
        print(
            (
                "%dth percentile in Method Size: %.2f"
                % (
                    int(args.percentile),
                    np.percentile(np.array(method_size_array), int(args.percentile)),
                )
            )
        )
        print(
            (
                "%dth percentile in Block Size: %.2f"
                % (
                    int(args.percentile),
                    np.percentile(np.array(block_size_array), int(args.percentile)),
                )
            )
        )
        print(
            (
                "Methods of size %d: %d"
                % (int(args.countSize), method_size_array.count(int(args.countSize)))
            )
        )
        # print method_size_array
        # print block_size_array
    else:
        print("No Traces Found")


main()
