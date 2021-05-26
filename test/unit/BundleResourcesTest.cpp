/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include "BundleResources.h"
#include "Debug.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "RedexTestUtils.h"
#include "Trace.h"

using namespace boost::filesystem;

namespace {

void copy_file(const std::string& from, const std::string& to) {
  std::ifstream src_stream(from, std::ios::binary);
  std::ofstream dest_stream(to, std::ios::binary);
  dest_stream << src_stream.rdbuf();
}

void setup_resources_and_run(
    const std::function<void(const std::string& extract_dir, BundleResources*)>&
        callback) {
  auto tmp_dir = redex::make_tmp_dir("BundleResourcesTest%%%%%%%%");
  boost::filesystem::path p(tmp_dir.path);

  auto manifest_dir = p / "base/manifest";
  create_directories(manifest_dir);
  copy_file(std::getenv("test_manifest_path"),
            manifest_dir.string() + "/AndroidManifest.xml");

  auto layout_dir = p / "base/res/layout";
  create_directories(layout_dir);
  auto layout_dest = layout_dir.string() + "/activity_main.xml";
  copy_file(std::getenv("test_layout_path"), layout_dest);

  BundleResources resources(tmp_dir.path);
  callback(tmp_dir.path, &resources);
}
} // namespace

TEST(BundleResources, TestReadMinSdk) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, BundleResources* resources) {
        auto result = resources->get_min_sdk();
        EXPECT_EQ(*result, 21);
      });
}
