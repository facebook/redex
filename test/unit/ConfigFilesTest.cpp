/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>
#include <sstream>
#include <unistd.h>

#include "ConfigFiles.h"
#include "RedexTest.h"

class ConfigFilesTest : public RedexTest {
 public:
  void validate_frequencies(
      const UnorderedMap<const DexString*, std::vector<uint8_t>>&
          class_freq_map,
      const std::string& class_name,
      const std::vector<uint8_t>& expected_frequencies) {
    std::vector<uint8_t> frequencies =
        class_freq_map.at(DexString::make_string(class_name));
    EXPECT_EQ(frequencies, expected_frequencies);
  }
};

TEST_F(ConfigFilesTest, read_class_frequencies) {
  auto* class_frequency_path = std::getenv("class_frequencies_path");

  Json::Value json_cfg;
  std::istringstream temp_json(
      "{\"redex\":{\"passes\":[]}, \"class_frequencies\": \"\"}");

  temp_json >> json_cfg;
  json_cfg["class_frequencies"] = class_frequency_path;
  ConfigFiles conf(json_cfg);

  const UnorderedMap<const DexString*, std::vector<uint8_t>>& class_freq_map =
      conf.get_class_frequencies();
  const std::vector<std::string> interactions = conf.get_interactions();
  validate_frequencies(class_freq_map,
                       "Lcom/facebook/redextest/ColdStart;",
                       {100, 2, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  validate_frequencies(
      class_freq_map, "Lcom/facebook/redextest/C1;", {99, 0, 94});
  validate_frequencies(class_freq_map,
                       "Lcom/facebook/redextest/C2;",
                       {71, 0, 70, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                        0,  0, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        0,  0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 3});
  validate_frequencies(class_freq_map,
                       "Lcom/facebook/redextest/C3;",
                       {91, 0, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3});
  validate_frequencies(class_freq_map,
                       "Lcom/facebook/redextest/C4;",
                       {66, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        0,  0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 14});

  EXPECT_EQ(interactions.at(0), "ColdStart");
  EXPECT_EQ(interactions.at(11), "000009");

  EXPECT_EQ(
      class_freq_map
          .at(DexString::make_string("Lcom/facebook/redextest/ColdStart;"))
          .at(0),
      100);
  EXPECT_EQ(
      class_freq_map.at(DexString::make_string("Lcom/facebook/redextest/C2;"))
          .at(11),
      1);
}

// Helper: write content to a temp file and return its path.
static std::string write_temp_dead_class_file(const std::string& content) {
  char tmpl[] = "/tmp/dead_class_test_XXXXXX";
  int fd = mkstemp(tmpl);
  EXPECT_NE(fd, -1);
  close(fd);
  std::ofstream out(tmpl);
  out << content;
  out.close();
  return tmpl;
}

// Helper: create a ConfigFiles with a dead_class_list pointing to the given
// file.
static ConfigFiles make_config_with_dead_class_list(const std::string& path) {
  Json::Value json_cfg;
  std::istringstream temp_json("{\"redex\":{\"passes\":[]}}");
  temp_json >> json_cfg;
  json_cfg["dead_class_list"] = path;
  return ConfigFiles(json_cfg);
}

// Standard 6-column format (old pipeline output).
TEST_F(ConfigFilesTest, dead_class_list_standard_format) {
  auto path = write_temp_dead_class_file(
      "com.facebook.Foo\t100\t50\t10\t3\t86400\n"
      "com.facebook.Bar\t200\t0\t0\t8\t172800\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 2);
  EXPECT_NE(dead.find("Lcom/facebook/Foo;"), dead.end());
  EXPECT_NE(dead.find("Lcom/facebook/Bar;"), dead.end());

  const auto& foo = dead.at("Lcom/facebook/Foo;");
  EXPECT_EQ(foo.sampled, 100);
  EXPECT_EQ(foo.unsampled, 50);
  EXPECT_EQ(foo.beta_unsampled, 10);
  EXPECT_EQ(foo.last_modified_count, 3);
  EXPECT_EQ(foo.seconds_since_last_modified, 86400);

  const auto& bar = dead.at("Lcom/facebook/Bar;");
  EXPECT_EQ(bar.sampled, 200);
  EXPECT_EQ(bar.seconds_since_last_modified, 172800);

  std::remove(path.c_str());
}

// "null" string in numeric column — should use struct default, not crash.
TEST_F(ConfigFilesTest, dead_class_list_null_string_value) {
  auto path = write_temp_dead_class_file(
      "com.facebook.NullTest\t100\t50\t10\t0\tnull\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 1);
  EXPECT_NE(dead.find("Lcom/facebook/NullTest;"), dead.end());
  const auto& entry = dead.at("Lcom/facebook/NullTest;");
  EXPECT_EQ(entry.sampled, 100);
  EXPECT_EQ(entry.last_modified_count, 0);
  // "null" is not parseable — should keep default value (0).
  EXPECT_EQ(entry.seconds_since_last_modified, 0);
}

// Quoted class names (new to_csv_file format) — quotes should be stripped.
TEST_F(ConfigFilesTest, dead_class_list_quoted_classname) {
  auto path = write_temp_dead_class_file(
      "\"com.facebook.Quoted\"\t100\t50\t10\t3\t86400\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 1);
  // Class name should have quotes stripped.
  EXPECT_NE(dead.find("Lcom/facebook/Quoted;"), dead.end());
  EXPECT_EQ(dead.at("Lcom/facebook/Quoted;").sampled, 100);

  std::remove(path.c_str());
}

// Extra trailing columns (date + app name from new pipeline) — should be
// ignored.
TEST_F(ConfigFilesTest, dead_class_list_extra_columns) {
  auto path = write_temp_dead_class_file(
      "com.facebook.Extra\t100\t50\t10\t3\t86400\t\"2026-04-"
      "04\"\t\"katana\"\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 1);
  EXPECT_NE(dead.find("Lcom/facebook/Extra;"), dead.end());
  EXPECT_EQ(dead.at("Lcom/facebook/Extra;").seconds_since_last_modified, 86400);

  std::remove(path.c_str());
}

// Fewer columns than expected — missing columns should keep struct defaults.
TEST_F(ConfigFilesTest, dead_class_list_fewer_columns) {
  // Only classname + sampled (2 columns instead of 6).
  auto path = write_temp_dead_class_file("com.facebook.Minimal\t100\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 1);
  EXPECT_NE(dead.find("Lcom/facebook/Minimal;"), dead.end());
  const auto& entry = dead.at("Lcom/facebook/Minimal;");
  EXPECT_EQ(entry.sampled, 100);
  // Rest should be struct defaults.
  EXPECT_EQ(entry.unsampled, 0);
  EXPECT_EQ(entry.beta_unsampled, 0);
  EXPECT_EQ(entry.last_modified_count, 1);
  EXPECT_EQ(entry.seconds_since_last_modified, 0);

  std::remove(path.c_str());
}

// Combined: new pipeline format with quotes, nulls, and extra columns.
TEST_F(ConfigFilesTest, dead_class_list_new_pipeline_format) {
  auto path = write_temp_dead_class_file(
      "\"com.facebook.Normal\"\t100\t50\t10\t3\t86400\t\"2026-04-"
      "04\"\t\"katana\"\n"
      "\"com.facebook.NullCol\"\t200\t30\t0\t0\tnull\t\"2026-04-"
      "04\"\t\"katana\"\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 2);
  // Quotes stripped, values parsed correctly.
  EXPECT_NE(dead.find("Lcom/facebook/Normal;"), dead.end());
  EXPECT_EQ(dead.at("Lcom/facebook/Normal;").seconds_since_last_modified,
            86400);
  // "null" → default 0.
  EXPECT_NE(dead.find("Lcom/facebook/NullCol;"), dead.end());
  EXPECT_EQ(dead.at("Lcom/facebook/NullCol;").seconds_since_last_modified, 0);

  std::remove(path.c_str());
}

// Empty file — should produce an empty dead class list.
TEST_F(ConfigFilesTest, dead_class_list_empty_file) {
  auto path = write_temp_dead_class_file("");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();

  EXPECT_EQ(dead.size(), 0);

  std::remove(path.c_str());
}

// Relocated class suffix — should go to live_class_split_list, not dead list.
TEST_F(ConfigFilesTest, dead_class_list_relocated_class) {
  auto path = write_temp_dead_class_file(
      "com.facebook.Alive$relocated\t100\t50\t10\t3\t86400\n"
      "com.facebook.Dead\t200\t0\t0\t8\t172800\n");
  auto conf = make_config_with_dead_class_list(path);
  const auto& dead = conf.get_dead_class_list();
  const auto& live = conf.get_live_class_split_list();

  EXPECT_EQ(dead.size(), 1);
  EXPECT_NE(dead.find("Lcom/facebook/Dead;"), dead.end());

  EXPECT_NE(live.find("Lcom/facebook/Alive;"), live.end());

  std::remove(path.c_str());
}
