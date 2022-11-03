/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>

#include "RedexTest.h"

class OSSConfigTest : public RedexIntegrationTest {
 public:
  void SetUp() override {}
};

TEST_F(OSSConfigTest, default_cfg) {
  auto config_file_env = std::getenv("default_config_file");
  always_assert_log(config_file_env != nullptr, "Config file is missing.");
  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;
  EXPECT_FALSE(cfg.empty());
}

TEST_F(OSSConfigTest, aggressive_cfg) {
  auto config_file_env = std::getenv("aggressive_config_file");
  always_assert_log(config_file_env != nullptr, "Config file is missing.");
  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;
  EXPECT_FALSE(cfg.empty());
}
