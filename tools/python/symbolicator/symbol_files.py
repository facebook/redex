# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import logging
import os
import re
import subprocess


##############################################################################
# Util functions
##############################################################################


def find_buck_artifacts(target):
    """
    Given a buck target, find the location of its build artifacts, which
    contain the symbol files.
    """
    root = subprocess.check_output(["buck", "root"]).strip()
    rule, output = subprocess.check_output(
        ["buck", "targets", "--show-output", target]
    ).split()
    re_match = re.match("//(.*_redex)", rule)

    artifacts = os.path.join(
        root, "buck-out", "gen", re_match.group(1).replace(":", "/") + "__redex"
    )
    return artifacts


##############################################################################


class SymbolFiles(object):
    def __init__(self, extracted_symbols, line_map, debug_line_map, iodi_metadata):
        self.extracted_symbols = extracted_symbols
        self.line_map = line_map
        self.debug_line_map = debug_line_map
        self.iodi_metadata = iodi_metadata

    @staticmethod
    def from_buck_artifact_dir(artifact_dir):
        line_map_fn_v1 = os.path.join(artifact_dir, "redex-line-number-map")
        line_map_fn_v2 = os.path.join(artifact_dir, "redex-line-number-map-v2")
        if os.path.exists(line_map_fn_v2):
            line_map_fn = line_map_fn_v2
        else:
            line_map_fn = line_map_fn_v1
        return SymbolFiles(
            os.path.join(artifact_dir, "redex-class-rename-map.txt"),
            line_map_fn,
            os.path.join(artifact_dir, "redex-debug-line-map-v2"),
            os.path.join(artifact_dir, "iodi-metadata"),
        )

    @staticmethod
    def from_buck_target(target):
        artifact_dir = find_buck_artifacts(target)
        logging.info("buck target %s has artifact dir at %s", target, artifact_dir)
        return SymbolFiles.from_buck_artifact_dir(artifact_dir)
