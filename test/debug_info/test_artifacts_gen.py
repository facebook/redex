#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os

from pyredex.packer import compress_entries, CompressionLevel
from redex import get_compression_list

from .artifacts_test_fixture import ArtifactsTestFixture


class TestArtifactsGeneration(ArtifactsTestFixture):

    ARTIFACTS_EXPECTED = {
        "redex-line-number-map-v2",
        "redex-debug-line-map-v2",
        "redex-class-id-map.txt",
        "redex-method-id-map.txt",
        "redex-method-id-map.txt.zst",
        "redex-class-rename-map.txt",
        "redex-class-method-info-map.txt",
        "redex-class-dependencies.txt.xz",
        "redex-opt-decisions.json",
        "iodi-metadata",
        "redex-unreachable-removed-symbols.txt",
        "redex-class-method-info-map.txt",
    }

    def setUp(self):
        super(TestArtifactsGeneration, self).setUp()
        self.config = {
            "redex": {
                "passes": [
                    "VerifierPass",
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

    def test_resource_mapping_compression(self):
        source_name = "resource-mapping.txt"
        output_name = "redex-resource-mapping.txt.zst"
        contents = b'{"res/layout/original.xml": "res/a.xml"}\n'
        source_path = os.path.join(self.tmp, source_name)

        entries = [
            entry
            for entry in get_compression_list()
            if entry.output_name == output_name
        ]
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertEqual(entry.file_list_must, [])
        self.assertEqual(entry.file_list_may, [source_name])
        self.assertFalse(entry.remove_source)
        self.assertEqual(entry.compression_level, CompressionLevel.BETTER)

        compress_entries(
            entries,
            self.tmp,
            self.tmp,
            argparse.Namespace(config=None),
            processes=1,
        )
        self.assert_artifact_does_not_exist(output_name)

        with open(source_path, "wb") as source:
            source.write(contents)

        compress_entries(
            entries,
            self.tmp,
            self.tmp,
            argparse.Namespace(config=None),
            processes=1,
        )

        self.assert_artifact_exists(output_name)
        self.assertGreater(os.path.getsize(os.path.join(self.tmp, output_name)), 0)
        with open(source_path, "rb") as source:
            self.assertEqual(source.read(), contents)
