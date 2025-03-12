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

// Asserts that the given loaded resources.arsc file has an overlayable entry
// with two policies.
#define ASSERT_OVERLAYABLES(res_table)                                         \
  ({                                                                           \
    auto _arsc_file = (ResourcesArscFile*)(res_table).get();                   \
    auto& _table_parser = _arsc_file->get_table_snapshot().get_parsed_table(); \
    EXPECT_EQ(_table_parser.m_package_overlayables.size(), 1)                  \
        << "Package expected to have two <overlayable> elements!";             \
    EXPECT_EQ(_table_parser.m_package_overlayables.begin()->second.size(), 2)  \
        << "Package expected to have two <overlayable> elements!";             \
  })
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

TEST(ApkResources, TestReadingWritingOverlays) {
  setup_resources_and_run([&](const std::string& temp_dir_path,
                              ApkResources* resources) {
    // Stash the original for comparisons.
    auto arsc_path =
        (boost::filesystem::path(temp_dir_path) / "resources.arsc").string();
    redex::copy_file(arsc_path, arsc_path + ".orig");
    auto res_table = resources->load_res_table();
    ASSERT_OVERLAYABLES(res_table);
    // Make a remapping that doesn't change anything, just to ensure builder
    // code is emitting the same file as was given.
    std::map<uint32_t, uint32_t> no_change_remapping;
    for (auto id : res_table->sorted_res_ids) {
      no_change_remapping.emplace(id, id);
    }
    res_table->remap_res_ids_and_serialize({}, no_change_remapping);
    EXPECT_TRUE(redex::are_files_equal(arsc_path, arsc_path + ".orig"))
        << "Round trip serialization is not equivalent!";
  });
}

TEST(ApkResources, TestRemappingOverlays) {
  setup_resources_and_run([&](const std::string& temp_dir_path,
                              ApkResources* resources) {
    // Stash the original for comparisons.
    auto arsc_path =
        (boost::filesystem::path(temp_dir_path) / "resources.arsc").string();
    auto arsc_size = boost::filesystem::file_size(arsc_path);
    auto res_table = resources->load_res_table();
    ASSERT_OVERLAYABLES(res_table);
    // Make a remapping that changes the last value in the binary file.
    std::map<uint32_t, uint32_t> remapping;
    for (auto id : res_table->sorted_res_ids) {
      remapping.emplace(id, id);
    }
    auto id = res_table->name_to_ids["yummy_orange"][0];
    constexpr uint32_t expected_value = 0x7f999999;
    remapping[id] = expected_value;
    res_table->remap_res_ids_and_serialize({}, remapping);
    // Verify the remapping took effect, which should rewrite the last 4 bytes
    // of the file.
    auto mapped_file = RedexMappedFile::open(arsc_path);
    auto id_ptr =
        (uint32_t*)(mapped_file.const_data() + arsc_size - sizeof(uint32_t));
    EXPECT_EQ(*id_ptr, expected_value) << "Last ID was not remapped!";
  });
}
