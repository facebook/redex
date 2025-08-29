/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include "ApkResources.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "ResourcesTestDefs.h"
#include "Trace.h"
#include "androidfw/ResourceTypes.h"
#include "arsc/TestStructures.h"
#include "utils/Serialize.h"

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
      [&](const std::string& /*extract_dir*/, ApkResources* resources) {
        auto result = resources->get_min_sdk();
        EXPECT_EQ(*result, 21);

        auto package_name = resources->get_manifest_package_name();
        EXPECT_STREQ(package_name->c_str(), "com.fb.bundles");
      });
}

TEST(ApkResources, ReadLayoutResolveRefs) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, ApkResources* resources) {
        UnorderedSet<std::string> layout_classes;
        UnorderedSet<std::string> attrs_to_read;
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
    // Check the correct ids are returned as roots.
    auto overlayables = res_table->get_overlayable_id_roots();
    EXPECT_EQ(overlayables.size(),
              sample_app::EXPECTED_OVERLAYABLE_RESOURCES.size());
    for (auto& name : sample_app::EXPECTED_OVERLAYABLE_RESOURCES) {
      EXPECT_TRUE(is_overlayable(name, res_table.get()))
          << name << " is not overlayable!";
    }
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
    auto* id_ptr =
        (uint32_t*)(mapped_file.const_data() + arsc_size - sizeof(uint32_t));
    EXPECT_EQ(*id_ptr, expected_value) << "Last ID was not remapped!";
  });
}

TEST(ApkResources, TestDeleteOverlayableIds) {
  // Make a hypothetical .arsc file with 3 dimensions, two of which are
  // overlayable. In a few steps the overlayable ids will be deleted, to verify
  // the overlayable header and policy shrinks, and is removed entirely.
  //
  // According to aapt2, it looks like the following:
  //
  // Binary APK
  // Package name=foo id=7f
  //   type dimen id=01 entryCount=3
  //     resource 0x7f010000 dimen/one
  //       () 10.000000dp
  //     resource 0x7f010001 dimen/two OVERLAYABLE
  //       () 20.000000dp
  //     resource 0x7f010002 dimen/three OVERLAYABLE
  //       () 30.000000dp
  auto global_strings_builder = std::make_shared<arsc::ResStringPoolBuilder>(
      android::ResStringPool_header::UTF8_FLAG);
  auto key_strings_builder = std::make_shared<arsc::ResStringPoolBuilder>(
      android::ResStringPool_header::UTF8_FLAG);
  key_strings_builder->add_string("one");
  key_strings_builder->add_string("two");
  key_strings_builder->add_string("three");
  auto type_strings_builder = std::make_shared<arsc::ResStringPoolBuilder>(0);
  type_strings_builder->add_string("dimen");

  auto package_builder =
      std::make_shared<arsc::ResPackageBuilder>(&foo_package);
  package_builder->set_key_strings(key_strings_builder);
  package_builder->set_type_strings(type_strings_builder);

  auto table_builder = std::make_shared<arsc::ResTableBuilder>();
  table_builder->set_global_strings(global_strings_builder);
  table_builder->add_package(package_builder);

  // dimen
  std::vector<android::ResTable_config*> dimen_configs{&default_config};
  std::vector<uint32_t> dimen_flags{0, 0, 0};
  auto dimen_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id,
      1,
      dimen_configs,
      dimen_flags,
      false /* enable_canonical_entries */,
      false /* enable_sparse_encoding */);
  package_builder->add_type(dimen_type_definer);

  // Add the three entries
  EntryAndValue one(0, android::Res_value::TYPE_DIMENSION, 0xa01 /* 10dp */);
  EntryAndValue two(1, android::Res_value::TYPE_DIMENSION, 0x1401 /* 20dp */);
  EntryAndValue three(2, android::Res_value::TYPE_DIMENSION, 0x1e01 /* 30dp */);
  dimen_type_definer->add(&default_config, &one);
  dimen_type_definer->add(&default_config, &two);
  dimen_type_definer->add(&default_config, &three);

  // Basic info to describe two overlayable ids.
  uint32_t initial_ids[2] = {0x7f010001, 0x7f010002};
  uint32_t policy_size =
      sizeof(android::ResTable_overlayable_policy_header) + sizeof(initial_ids);

  android::ResTable_overlayable_policy_header policy{};
  policy.header.type = android::RES_TABLE_OVERLAYABLE_POLICY_TYPE;
  policy.header.headerSize =
      sizeof(android::ResTable_overlayable_policy_header);
  policy.header.size = policy_size;
  policy.entry_count = 2;
  policy.policy_flags = android::ResTable_overlayable_policy_header::SIGNATURE;

  android::ResTable_overlayable_header overlayable{};
  overlayable.header.type = android::RES_TABLE_OVERLAYABLE_TYPE;
  overlayable.header.headerSize = sizeof(android::ResTable_overlayable_header);
  overlayable.header.size =
      sizeof(android::ResTable_overlayable_header) + policy_size;
  overlayable.name[0] = 'y';
  overlayable.name[1] = 'o';

  arsc::OverlayInfo overlay_info(&overlayable);
  overlay_info.policies.emplace(&policy, initial_ids);
  package_builder->add_overlay(overlay_info);

  android::Vector<char> table_data;
  table_builder->serialize(&table_data);

  // Simple function to assert the number of overlayable related headers parsed.
  auto run_verify = [](ResourceTableFile* res_table,
                       size_t overlayable_count,
                       size_t policy_count,
                       const std::vector<uint32_t>& expected_ids) {
    auto* arsc_table = (ResourcesArscFile*)res_table;
    auto& parsed_table = arsc_table->get_table_snapshot().get_parsed_table();
    auto& parsed_overlays_map =
        parsed_table.m_package_overlayables.begin()->second;
    EXPECT_EQ(parsed_overlays_map.size(), overlayable_count)
        << "Incorrect size of overlayable headers";
    if (overlayable_count > 0) {
      auto* header = parsed_overlays_map.begin()->first;
      auto& parsed_info = parsed_overlays_map.begin()->second;
      EXPECT_EQ(parsed_info.policies.size(), policy_count)
          << "Incorrect size of policy headers";
      auto* policy_header = parsed_info.policies.begin()->first;
      EXPECT_EQ(policy_header->entry_count, expected_ids.size())
          << "Incorrect number of overlayable ids!";
      EXPECT_EQ(policy_header->header.size,
                sizeof(android::ResTable_overlayable_policy_header) +
                    expected_ids.size() * sizeof(uint32_t))
          << "Policy header size is incorrect.";
      EXPECT_EQ(header->header.size,
                sizeof(android::ResTable_overlayable_header) +
                    policy_header->header.size)
          << "Overlayable header size is incorrect.";
      auto* parsed_ids = parsed_info.policies.begin()->second;
      for (size_t i = 0; i < expected_ids.size(); i++) {
        EXPECT_EQ(parsed_ids[i], expected_ids[i])
            << "Incorrect ID at index " << i;
      }
    }
  };

  // Parse the above file and start deleting from it.
  auto tmp_dir = redex::make_tmp_dir("ApkResourcesTest%%%%%%%%");
  boost::filesystem::path tmp_path(tmp_dir.path);
  arsc::write_bytes_to_file(table_data, (tmp_path / "resources.arsc").string());

  // Base state
  {
    ApkResources resources(tmp_dir.path);
    auto res_table = resources.load_res_table();
    run_verify(res_table.get(), 1, 1, {0x7f010001, 0x7f010002});

    // Delete 0x7f010002
    res_table->delete_resource(0x7f010002);
    std::map<uint32_t, uint32_t> remapping{{0x7f010000, 0x7f010000},
                                           {0x7f010001, 0x7f010001}};
    res_table->remap_res_ids_and_serialize({}, remapping);
  }

  // After first deletion, file should look like this:
  //
  // Binary APK
  // Package name=foo id=7f
  //   type dimen id=01 entryCount=2
  //     resource 0x7f010000 dimen/one
  //       () 10.000000dp
  //     resource 0x7f010001 dimen/two OVERLAYABLE
  //       () 20.000000dp
  {
    ApkResources resources(tmp_dir.path);
    auto res_table = resources.load_res_table();
    run_verify(res_table.get(), 1, 1, {0x7f010001});

    // Delete 0x7f010001
    res_table->delete_resource(0x7f010001);
    std::map<uint32_t, uint32_t> remapping{{0x7f010000, 0x7f010000}};
    res_table->remap_res_ids_and_serialize({}, remapping);
  }

  // After second deletion, file should look like this:
  //
  // Binary APK
  // Package name=foo id=7f
  //   type dimen id=01 entryCount=1
  //     resource 0x7f010000 dimen/one
  //       () 10.000000dp
  {
    ApkResources resources(tmp_dir.path);
    auto res_table = resources.load_res_table();
    run_verify(res_table.get(), 0, 0, {});
  }
}
