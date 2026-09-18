/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexOutput.h"
#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>

TEST(DexOutput, checkMethodInstructionSizeLimit) {

  Json::Value json_cfg;
  std::istringstream temp_json(
      "{\"redex\":{\"passes\":[]}, \"instruction_size_bitwidth_limit\": 0}");

  temp_json >> json_cfg;
  json_cfg["instruction_size_bitwidth_limit"] = 16;
  ConfigFiles conf(json_cfg);
  bool described = false;
  EXPECT_NO_THROW(DexOutput::check_method_instruction_size_limit(
      conf, 65536, [&described]() {
        described = true;
        return std::string("method");
      }));
  EXPECT_FALSE(described);
  EXPECT_THROW(DexOutput::check_method_instruction_size_limit(
                   conf,
                   65537,
                   [&described]() {
                     described = true;
                     return std::string("method");
                   }),
               RedexException);
  EXPECT_TRUE(described);
}

TEST(DexOutput, rejectsNegativeSizeWithoutConfiguredLimit) {
  Json::Value json_cfg;
  std::istringstream temp_json(
      "{\"redex\":{\"passes\":[]}, \"instruction_size_bitwidth_limit\": 0}");
  temp_json >> json_cfg;
  ConfigFiles conf(json_cfg);

  bool described = false;
  EXPECT_THROW(DexOutput::check_method_instruction_size_limit(
                   conf,
                   -1,
                   [&described]() {
                     described = true;
                     return std::string("method");
                   }),
               RedexException);
  EXPECT_TRUE(described);
}
