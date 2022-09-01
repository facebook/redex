/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <unordered_set>

#include "ApkResources.h"
#include "Debug.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "RedexTestUtils.h"
#include "Trace.h"

using namespace boost::filesystem;

namespace {
// Make a temp dir and copy of input manifest file. Used to allow test cases
// to modify the file without interfering with other runs of the test.
void setup_resources_and_run(
    const std::function<void(const std::string& extract_dir, ApkResources*)>&
        callback) {
  auto tmp_dir = redex::make_tmp_dir("ApkResourcesTest%%%%%%%%");
  boost::filesystem::path p(tmp_dir.path);
  redex::copy_file(std::getenv("test_manifest_path"),
                   (p / "AndroidManifest.xml").string());
  ApkResources resources(tmp_dir.path);
  callback(tmp_dir.path, &resources);
}
} // namespace

TEST(ApkResources, TestReadManifest) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, ApkResources* resources) {
        auto result = resources->get_min_sdk();
        EXPECT_EQ(*result, 21);

        auto package_name = resources->get_manifest_package_name();
        EXPECT_STREQ(package_name->c_str(), "com.fb.bundles");
      });
}
