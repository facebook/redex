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
#include "RedexTest.h"
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
  redex::copy_file(get_env("test_manifest_path"),
                   (p / "AndroidManifest.xml").string());
  redex::copy_file(get_env("test_res_path"), (p / "resources.arsc").string());

  auto layout_dir = p / "res/layout";
  create_directories(layout_dir);
  auto layout_dest = layout_dir.string() + "/activity_main.xml";
  redex::copy_file(get_env("test_layout_path"), layout_dest);

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

TEST(ApkResources, ReadLayoutResolveRefs) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, ApkResources* resources) {
        std::unordered_set<std::string> layout_classes;
        std::unordered_set<std::string> attrs_to_read;
        attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
        std::unordered_multimap<std::string, std::string> attribute_values;
        resources->collect_layout_classes_and_attributes(
            attrs_to_read, &layout_classes, &attribute_values);

        EXPECT_EQ(layout_classes.size(), 4);
        EXPECT_EQ(attribute_values.size(), 2);

        // One reference should have been resolved to two possible classes.
        EXPECT_EQ(layout_classes.count("A"), 1);
        EXPECT_EQ(layout_classes.count("B"), 1);
        EXPECT_EQ(layout_classes.count("com.fb.bundles.WickedCoolButton"), 1);
        EXPECT_EQ(layout_classes.count("com.fb.bundles.NiftyViewGroup"), 1);
      });
}
