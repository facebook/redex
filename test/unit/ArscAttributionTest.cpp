/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <gtest/gtest.h>

#include "ArscStats.h"
#include "Debug.h"
#include "RedexTestUtils.h"
#include "Util.h"
#include "androidfw/ResourceTypes.h"
#include "arsc/TestStructures.h"
#include "utils/Errors.h"
#include "utils/Serialize.h"

namespace {
constexpr uint32_t UTF8_POOL = android::ResStringPool_header::UTF8_FLAG;
constexpr uint32_t UTF16_POOL = 0;
attribution::ResourceNames no_names;
} // namespace

// clang-format off
// This test case builds up an .arsc file that should be counted like this:
//
// ID         | Type  | Name   | Private Size | Shared Size | Proportional Size | Config Count | Configs
// 0x7f010000 | dimen | yolo   | 148          | 0           | 247.33            | 2            | default land
// 0x7f010001 | dimen | second | 37           | 0           | 136.33            | 1            | default
// 0x7f010002 | dimen | third  | 36           | 0           | 135.33            | 1            | default
// 0x7f020000 | style | fourth | 179          | 0           | 239               | 1            | xxhdpi
// 0x7f030000 | xml   | fifth  | 52           | 0           | 184.50            | 1            | default
// 0x7f030001 | xml   | sixth  | 53           | 0           | 185.50            | 1            | default
//
// clang-format on
TEST(Arsc, BuildFileForAttribution) {
  auto global_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  global_strings_builder->add_string("res/a.xml");
  global_strings_builder->add_string("res/bb.xml");

  auto key_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  std::vector<std::string> entry_names{"first",  "second", "third",
                                       "fourth", "fifth",  "sixth"};
  for (const auto& s : entry_names) {
    key_strings_builder->add_string(s);
  }

  auto type_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF16_POOL);
  std::vector<std::string> type_names{"dimen", "style", "xml"};
  for (const auto& s : type_names) {
    type_strings_builder->add_string(s);
  }

  auto package_builder =
      std::make_shared<arsc::ResPackageBuilder>(&foo_package);
  package_builder->set_key_strings(key_strings_builder);
  package_builder->set_type_strings(type_strings_builder);

  auto table_builder = std::make_shared<arsc::ResTableBuilder>();
  table_builder->set_global_strings(global_strings_builder);
  table_builder->add_package(package_builder);

  // dimen
  std::vector<android::ResTable_config*> dimen_configs = {&default_config,
                                                          &land_config};
  // First res ID has entries in two different configs (this flag denotes that).
  // Subsequent two entries only have default config entries (hence zero).
  std::vector<uint32_t> dimen_flags = {
      android::ResTable_config::CONFIG_ORIENTATION, 0, 0};
  auto dimen_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id, 1, dimen_configs, dimen_flags);
  package_builder->add_type(dimen_type_definer);

  dimen_type_definer->add(&default_config, &e0);
  dimen_type_definer->add(&land_config, &e0_land);
  dimen_type_definer->add(&default_config, &e1);
  dimen_type_definer->add_empty(&land_config);
  dimen_type_definer->add(&default_config, &e2);
  dimen_type_definer->add_empty(&land_config);

  // style
  std::vector<android::ResTable_config*> style_configs = {&xxhdpi_config};
  std::vector<uint32_t> style_flags = {
      android::ResTable_config::CONFIG_DENSITY};
  auto style_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id, 2, style_configs, style_flags);
  package_builder->add_type(style_type_definer);

  style.item0.name.ident = 0x01010098; // android:textColor
  style.item0.value.dataType = android::Res_value::TYPE_INT_COLOR_RGB8;
  style.item0.value.data = 0xFF0000FF;

  style.item1.name.ident = 0x010100d4; // android:background
  style.item1.value.dataType = android::Res_value::TYPE_INT_COLOR_RGB8;
  style.item1.value.data = 0xFF00FF00;

  style_type_definer->add(&xxhdpi_config, &style);

  // xml
  std::vector<android::ResTable_config*> xml_configs = {&default_config};
  std::vector<uint32_t> xml_flags = {0, 0};
  auto xml_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id, 3, xml_configs, xml_flags);
  package_builder->add_type(xml_type_definer);
  EntryAndValue x0(4, android::Res_value::TYPE_STRING, 0);
  EntryAndValue x1(5, android::Res_value::TYPE_STRING, 1);
  xml_type_definer->add(&default_config, &x0);
  xml_type_definer->add(&default_config, &x1);

  android::Vector<char> table_data;
  table_builder->serialize(&table_data);

  // Make a fake rename map.
  attribution::ResourceNames names{{0x7f010000, "yolo"}};
  attribution::ArscStats stats(table_data.array(), table_data.size(), names);
  auto results = stats.compute();
  EXPECT_EQ(results.size(), entry_names.size());
  std::vector<size_t> expected_private_sizes{148, 37, 36, 179, 52, 53};
  // For ease of comparison, these are the floor of expected values.
  std::vector<size_t> expected_proportional_sizes{247, 136, 135, 239, 184, 185};
  size_t idx = 0;
  for (const auto& result : results) {
    if (idx == 0) {
      // Make sure the given rename map takes priority.
      EXPECT_STREQ(result.name.c_str(), "yolo")
          << "Incorrect name for 0x" << std::hex << result.id;
      EXPECT_EQ(result.configs.size(), 2);
    } else {
      EXPECT_EQ(result.name, entry_names.at(idx))
          << "Incorrect name for 0x" << std::hex << result.id;
      EXPECT_EQ(result.configs.size(), 1);
    }
    EXPECT_EQ(result.sizes.private_size, expected_private_sizes.at(idx))
        << "Incorrect size for 0x" << std::hex << result.id;
    EXPECT_EQ(result.sizes.shared_size, 0);
    EXPECT_EQ((size_t)std::floor(result.sizes.proportional_size),
              expected_proportional_sizes.at(idx))
        << "Incorrect proportional size for 0x" << std::hex << result.id;
    idx++;
  }
}
