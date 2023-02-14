#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from .artifacts_test_fixture import ArtifactsTestFixture


class TestArtifactsGeneration(ArtifactsTestFixture):

    ARTIFACTS_EXPECTED = {
        "redex-line-number-map-v2",
        "redex-debug-line-map-v2",
        "redex-class-id-map.txt",
        "redex-method-id-map.txt",
        "redex-class-rename-map.txt",
        "redex-class-method-info-map.txt",
        "redex-class-dependencies.txt.xz",
        "redex-opt-decisions.json",
        "iodi-metadata",
        "redex-merge-interface-mappings.txt",
        "redex-unreachable-removed-symbols.txt",
        "redex-class-method-info-map.txt",
    }

    def setUp(self):
        super(TestArtifactsGeneration, self).setUp()
        self.config = {
            "redex": {
                "passes": [
                    "VerifierPass",
                    "MergeInterfacePass",
                    "RemoveUnreachablePass",
                    "RegAllocPass",
                ]
            },
            "opt_decisions": {"enable_logs": True},
            "debug_info_kind": "iodi",
            "emit_class_method_info_map": True,
        }

    def test_artifacts_gen(self):
        self.run_redex()
        for fn in self.ARTIFACTS_EXPECTED:
            self.assert_artifact_exists(fn)
