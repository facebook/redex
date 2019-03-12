/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexOutput.h"
#include <gtest/gtest.h>
#include <json/json.h>

TEST(DexOutput, checkMethodInstructionSizeLimit) {

  Json::Value json_cfg;
  std::istringstream temp_json(
      "{\"redex\":{\"passes\":[]}, \"instruction_size_bitwidth_limit\": 0}");

  temp_json >> json_cfg;
  json_cfg["instruction_size_bitwidth_limit"] = 16;
  ConfigFiles cfg(json_cfg);
  EXPECT_NO_THROW(
      DexOutput::check_method_instruction_size_limit(cfg, 65536, "method"));
  EXPECT_THROW(
      DexOutput::check_method_instruction_size_limit(cfg, 65537, "method"),
      std::runtime_error);
}
