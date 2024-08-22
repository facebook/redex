/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>

#include "ConfigFiles.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Show.h"

class ConfigFilesTest : public RedexTest {
 public:
  void validate_frequencies(
      const std::unordered_map<const DexString*, std::vector<uint8_t>>&
          class_freq_map,
      const std::string& class_name,
      const std::vector<uint8_t>& expected_frequencies) {
    std::vector<uint8_t> frequencies =
        class_freq_map.at(DexString::make_string(class_name));
    EXPECT_EQ(frequencies, expected_frequencies);
  }
};

TEST_F(ConfigFilesTest, read_class_frequencies) {
  auto class_frequency_path = std::getenv("class_frequencies_path");

  Json::Value json_cfg;
  std::istringstream temp_json(
      "{\"redex\":{\"passes\":[]}, \"class_frequencies\": \"\"}");

  temp_json >> json_cfg;
  json_cfg["class_frequencies"] = class_frequency_path;
  ConfigFiles conf(json_cfg);

  const std::unordered_map<const DexString*, std::vector<uint8_t>>&
      class_freq_map = conf.get_class_frequencies();
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
