# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import shutil
import subprocess
import tempfile
import unittest


class TestDebugInfoKind(unittest.TestCase):

    """
    This test suite checks that Redex emits the right symbol files for each
    possible setting of debug_info_kind.
    """

    CLASS_MAPPING = "redex-class-id-map.txt"
    DEBUG_LINE_MAP = "redex-debug-line-map-v2"
    IODI_METADATA = "iodi-metadata"
    LINE_NUMBER_MAP = "redex-line-number-map-v2"
    METHOD_MAPPING = "redex-method-id-map.txt"

    POSSIBLE_SYMBOL_FILES = {
        CLASS_MAPPING,
        DEBUG_LINE_MAP,
        IODI_METADATA,
        LINE_NUMBER_MAP,
        METHOD_MAPPING,
    }

    def setUp(self):
        self.config = {"redex": {"passes": ["RegAllocPass"]}}
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_redex(self):
        config_file = os.path.join(self.tmp, "config")
        with open(config_file, "w") as f:
            json.dump(self.config, f)

        subprocess.check_call(
            [
                os.environ["REDEX_SCRIPT"],
                "-P",
                os.environ["PG_CONFIG"],
                "-c",
                config_file,
                "--redex-binary",
                os.environ["REDEX_BINARY"],
                "-o",
                os.path.join(self.tmp, "out.apk"),
                os.environ["INPUT_APK"],
            ]
        )

    def check_artifacts(self, should_exist):
        for fn in should_exist:
            path = os.path.join(self.tmp, fn)
            self.assertTrue(os.path.exists(path), "%s does not exist" % fn)

        for fn in self.POSSIBLE_SYMBOL_FILES - should_exist:
            path = os.path.join(self.tmp, fn)
            self.assertFalse(os.path.exists(path), "%s exists" % fn)

    def test_no_custom_symbolication(self):
        self.run_redex()
        self.check_artifacts(set())

    def test_per_method_debug(self):
        self.config["debug_info_kind"] = "per_method_debug"
        self.run_redex()
        self.check_artifacts(
            {self.LINE_NUMBER_MAP, self.CLASS_MAPPING, self.METHOD_MAPPING}
        )

    def test_no_positions(self):
        self.config["debug_info_kind"] = "no_positions"
        self.run_redex()
        self.check_artifacts(
            {
                self.LINE_NUMBER_MAP,
                self.DEBUG_LINE_MAP,
                self.CLASS_MAPPING,
                self.METHOD_MAPPING,
            }
        )

    def test_iodi(self):
        self.config["debug_info_kind"] = "iodi"
        self.run_redex()
        self.check_artifacts(
            {
                self.LINE_NUMBER_MAP,
                self.DEBUG_LINE_MAP,
                self.IODI_METADATA,
                self.CLASS_MAPPING,
                self.METHOD_MAPPING,
            }
        )

    def test_iodi_per_arity(self):
        self.config["debug_info_kind"] = "iodi_per_arity"
        self.run_redex()
        self.check_artifacts(
            {
                self.LINE_NUMBER_MAP,
                self.DEBUG_LINE_MAP,
                self.IODI_METADATA,
                self.CLASS_MAPPING,
                self.METHOD_MAPPING,
            }
        )
