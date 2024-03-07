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
// The following accounts for null zero, utf-8 size (one byte) and utf-16
// size (one byte) for strings with small lengths.
constexpr size_t SIZE_AND_NULL_ZERO = 3;
constexpr size_t OFFSET_SIZE = sizeof(uint32_t);
attribution::ResourceNames no_names;

// Makes a string pool and calls the API method to count padding bytes.
size_t count_padding(const std::vector<std::string>& items,
                     uint32_t pool_flags) {
  arsc::ResStringPoolBuilder builder(pool_flags);
  for (const auto& s : items) {
    builder.add_string(s);
  }
  android::Vector<char> pool_data;
  builder.serialize(&pool_data);
  auto pool_header = (android::ResStringPool_header*)pool_data.array();
  android::ResStringPool pool;
  pool.setTo(pool_header, pool_data.size(), true);
  return attribution::count_padding(pool_header, pool);
}
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

TEST(Arsc, StringSpanAttribution) {
  // Make a string pool with two strings, first being a styled string with 2
  // html style tags and the second being a regular string. In human readable
  // form it looks like this:
  // clang-format off
  //
  // Package Groups (1)
  // Package Group 0 id=0x7f packageCount=1 name=foo
  //   Package 0 id=0x7f name=foo
  //     type 0 configCount=1 entryCount=2
  //       spec resource 0x7f010000 foo:string/first: flags=0x00000000
  //       spec resource 0x7f010001 foo:string/second: flags=0x00000000
  //       config (default):
  //         resource 0x7f010000 foo:string/first: t=0x03 d=0x00000000 (s=0x0008 r=0x00)
  //           (string8) "I like a fine glass of H20 in the morning."
  //         resource 0x7f010001 foo:string/second: t=0x03 d=0x00000001 (s=0x0008 r=0x00)
  //           (string8) "regular string"
  // clang-format on
  auto global_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  android::ResStringPool_span em{.name = {2}, .firstChar = 9, .lastChar = 12};
  android::ResStringPool_span sub{.name = {3}, .firstChar = 24, .lastChar = 24};
  std::string styled_string("I like a fine glass of H20 in the morning.");
  std::string regular_string("regular string");
  global_strings_builder->add_style(styled_string, {&em, &sub});
  global_strings_builder->add_string(regular_string);
  global_strings_builder->add_string("em");
  global_strings_builder->add_string("sub");

  // Check some things regarding the pool itself
  android::Vector<char> pool_data;
  global_strings_builder->serialize(&pool_data);
  auto pool_header = (android::ResStringPool_header*)pool_data.array();
  android::ResStringPool pool;
  pool.setTo(pool_header, pool_data.size(), true);
  EXPECT_EQ(attribution::count_padding(pool_header, pool), 3);

  // This API call is just for the bytes of styled_string itself.
  EXPECT_EQ(attribution::compute_string_character_size(pool, 0),
            styled_string.size() + SIZE_AND_NULL_ZERO);
  // Entire size to represent styled_string which includes an offset for it,
  // offsets for the 2 html tag names, as well as the size of the span
  // information for where the tags should be plus another offset to where the
  // span information starts.
  auto styled_string_data_size =
      styled_string.size() + strlen("em") + strlen("sub") +
      3 * SIZE_AND_NULL_ZERO + 3 * OFFSET_SIZE +
      2 * sizeof(android::ResStringPool_span) +
      sizeof(android::ResStringPool_span::END) + OFFSET_SIZE;
  EXPECT_EQ(attribution::compute_string_size(pool, 0), styled_string_data_size);

  // Just the bytes of regular_string
  EXPECT_EQ(attribution::compute_string_character_size(pool, 1),
            regular_string.size() + SIZE_AND_NULL_ZERO);
  // Entire size to represent regular_string which includes an offset.
  EXPECT_EQ(attribution::compute_string_size(pool, 1),
            regular_string.size() + SIZE_AND_NULL_ZERO + OFFSET_SIZE);

  // Continue on to build a full .arsc file and get the stats.
  auto key_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  key_strings_builder->add_string("first");
  key_strings_builder->add_string("second");

  auto type_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF16_POOL);
  type_strings_builder->add_string("string");

  auto package_builder =
      std::make_shared<arsc::ResPackageBuilder>(&foo_package);
  package_builder->set_key_strings(key_strings_builder);
  package_builder->set_type_strings(type_strings_builder);

  auto table_builder = std::make_shared<arsc::ResTableBuilder>();
  table_builder->set_global_strings(global_strings_builder);
  table_builder->add_package(package_builder);

  // string type
  std::vector<android::ResTable_config*> string_configs = {&default_config};
  std::vector<uint32_t> string_flags = {0, 0};
  auto string_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id, 1, string_configs, string_flags);
  package_builder->add_type(string_type_definer);
  EntryAndValue s0(0, android::Res_value::TYPE_STRING, 0);
  EntryAndValue s1(1, android::Res_value::TYPE_STRING, 1);
  string_type_definer->add(&default_config, &s0);
  string_type_definer->add(&default_config, &s1);

  android::Vector<char> table_data;
  table_builder->serialize(&table_data);

  attribution::ArscStats stats(table_data.array(), table_data.size(), no_names);
  auto results = stats.compute();
  EXPECT_EQ(results.size(), 2);

  const auto& result = results.at(0);
  auto size_of_key_string = OFFSET_SIZE + strlen("first") + SIZE_AND_NULL_ZERO;
  EXPECT_EQ(result.sizes.private_size,
            styled_string_data_size + size_of_key_string +
                OFFSET_SIZE /* typeSpec flag */ +
                OFFSET_SIZE /* type offset */ + sizeof(android::Res_value) +
                sizeof(android::ResTable_entry));
}

TEST(Arsc, CountPadding) {
  std::vector<std::string> odd = {"array"};
  std::vector<std::string> even = {"string"};
  EXPECT_EQ(count_padding(odd, UTF16_POOL), 2);
  EXPECT_EQ(count_padding(even, UTF16_POOL), 0);
}

TEST(Arsc, DuplicateDataAttribution) {
  auto global_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  auto key_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF8_POOL);
  key_strings_builder->add_string("(name removed)");
  auto type_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(UTF16_POOL);
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
  std::vector<uint32_t> dimen_flags{0, 0};
  auto dimen_type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
      foo_package.id,
      1,
      dimen_configs,
      dimen_flags,
      true /* enable_canonical_entries */,
      true /* enable_sparse_encoding */);
  package_builder->add_type(dimen_type_definer);

  EntryAndValue duplicate(0, android::Res_value::TYPE_DIMENSION, 9999);
  dimen_type_definer->add(&default_config, &duplicate);
  dimen_type_definer->add(&default_config, &duplicate);

  android::Vector<char> table_data;
  table_builder->serialize(&table_data);

  attribution::ArscStats stats(table_data.array(), table_data.size(), no_names);
  auto results = stats.compute();
  EXPECT_EQ(results.size(), 2);

  auto expected_shared_size =
      sizeof(android::Res_value) + sizeof(android::ResTable_entry) +
      strlen("(name removed)") + SIZE_AND_NULL_ZERO + OFFSET_SIZE;
  auto& first_result = results.at(0);
  EXPECT_EQ(first_result.sizes.shared_size, expected_shared_size);
  EXPECT_EQ(first_result.sizes.private_size, 2 * OFFSET_SIZE);

  // They are sharing same data and string name.
  auto& second_result = results.at(1);
  EXPECT_EQ(second_result.sizes.shared_size, expected_shared_size);
  EXPECT_EQ(second_result.sizes.private_size, 2 * OFFSET_SIZE);
}
