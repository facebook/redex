/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>
#include <sstream>

#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Verifier.h"

namespace fs = boost::filesystem;

constexpr const char* ARTIFACTS_FILENAME = "redex-class-dependencies.txt";

class VerifierArtifactsTest : public RedexIntegrationTest {};

TEST_F(VerifierArtifactsTest, file_exists) {
  auto scope = build_class_scope(stores);
  run_passes({
      new VerifierPass(),
  });
  fs::path out_dir(get_configfiles_out_dir());
  fs::path artifacts_path = out_dir / "meta" / ARTIFACTS_FILENAME;
  ASSERT_TRUE(boost::filesystem::exists(artifacts_path));
  // Simple sanity check on file contents
  std::vector<std::string> lines;
  std::ifstream infile(artifacts_path.c_str());
  std::string line;
  while (std::getline(infile, line)) {
    lines.emplace_back(line);
  }
  EXPECT_EQ(lines.size(), 2) << "Expected only two class refs";
  std::sort(lines.begin(), lines.end());
  EXPECT_TRUE(lines[0].find("Lredex/B;") < lines[0].find("Lredex/A;"));
  EXPECT_TRUE(lines[1].find("Lredex/VerifierTest;") <
              lines[1].find("Lredex/B;"));
}
