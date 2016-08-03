/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"

#include "ProguardLoader.h"


TEST(LegacyParserTest, wildcard) {
  g_redex = new RedexContext();
  const char* pgfile = std::getenv("pg");
  std::vector<KeepRule> rules;
  std::set<std::string> library_jars;
  bool parsed_ok = load_proguard_config_file(
         pgfile, &rules, &library_jars);
  ASSERT_TRUE(parsed_ok);
  delete g_redex;
}
