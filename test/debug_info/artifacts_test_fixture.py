#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest


class ArtifactsTestFixture(unittest.TestCase):
    def setUp(self):
        self.config = {"redex": {"passes": ["RegAllocPass"]}}
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_redex(self):
        config_file = os.path.join(self.tmp, "config")
        with open(config_file, "w") as f:
            json.dump(self.config, f)

        # Remove BUCK_EVENT_PIPE if set.
        if "BUCK_EVENT_PIPE" in os.environ:
            env = os.environ.copy()
            del env["BUCK_EVENT_PIPE"]
        else:
            env = os.environ

        try:
            subprocess.run(
                shlex.split(os.environ["REDEX_SCRIPT"])
                + [
                    "-P",
                    os.environ["PG_CONFIG"],
                    "-c",
                    config_file,
                    "-j",
                    f"{os.environ['SDK_PATH']}/platforms/{os.environ['SDK_TARGET']}/android.jar",
                    "--redex-binary",
                    os.environ["REDEX_BINARY"],
                    "-o",
                    os.path.join(self.tmp, "out.apk"),
                    # We don't care about zipalign or apk signing.
                    "--ignore-zipalign",
                    "--no-sign",
                    os.environ["INPUT_APK"],
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                # Move to text= when moving to Python > 3.6.
                universal_newlines=True,
                env=env,
            )
        except subprocess.CalledProcessError as e:
            print(f"stdout:\n{e.stdout}\n---\nstderr:\n{e.stderr}")
            raise e

    def assert_artifact_exists(self, filename):
        path = os.path.join(self.tmp, filename)
        self.assertTrue(os.path.exists(path), "%s does not exist" % filename)

    def assert_artifact_does_not_exist(self, filename):
        path = os.path.join(self.tmp, filename)
        self.assertFalse(os.path.exists(path), "%s exists" % filename)
