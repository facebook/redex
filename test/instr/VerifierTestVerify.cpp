/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

namespace fs = boost::filesystem;

#include "VerifyUtil.h"

constexpr const char* ARTIFACTS_FILENAME = "redex-class-dependencies.txt";

TEST_F(PostVerify, VerifierArtifactsGenerationTest) {
  const char* apk = std::getenv("apk");
  fs::path artifacts_path(apk);
  artifacts_path.remove_filename();
  artifacts_path /= "verifier_redex__redex";
  artifacts_path /= ARTIFACTS_FILENAME;
  ASSERT_TRUE(boost::filesystem::exists(artifacts_path));
}
