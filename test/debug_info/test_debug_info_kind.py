#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from .artifacts_test_fixture import ArtifactsTestFixture


class TestDebugInfoKind(ArtifactsTestFixture):
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

    def check_artifacts(self, should_exist):
        for fn in should_exist:
            self.assert_artifact_exists(fn)

        for fn in self.POSSIBLE_SYMBOL_FILES - should_exist:
            self.assert_artifact_does_not_exist(fn)

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

    def test_iodi_layered(self):
        self.config["debug_info_kind"] = "iodi2"
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
