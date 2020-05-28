/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <json/json.h>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "PassManager.h"
#include "RedexContext.h"
#ifndef IS_REDEX_TEST_LIBRARY
#include "SanitizersConfig.h"
#endif

struct RedexTest : public testing::Test {
  RedexTest() { g_redex = new RedexContext(); }

  ~RedexTest() { delete g_redex; }
};

struct RedexIntegrationTest : public RedexTest {
 protected:
  const char* dex_file;
  const char* secondary_dex_file;
  std::vector<DexStore> stores;
  boost::optional<DexClasses&> classes;
  DexMetadata dex_metadata;

 public:
  RedexIntegrationTest() {
    dex_file = std::getenv("dexfile");

    always_assert_log(dex_file,
                      "Dex file must be set up before integration tests.\n");

    secondary_dex_file = std::getenv("secondary_dexfile");

    dex_metadata.set_id("classes");
    DexStore root_store(dex_metadata);
    root_store.add_classes(load_classes_from_dex(dex_file));
    if (secondary_dex_file) {
      root_store.add_classes(load_classes_from_dex(secondary_dex_file));
    }
    classes = root_store.get_dexen().back();
    stores.emplace_back(std::move(root_store));
  }

  void run_passes(
      const std::vector<Pass*>& passes,
      std::unique_ptr<keep_rules::ProguardConfiguration> pg_config = nullptr,
      const Json::Value& json_conf = Json::nullValue) {

    std::unique_ptr<PassManager> manager = nullptr;
    if (pg_config) {
      manager = std::make_unique<PassManager>(
          passes, std::move(pg_config), json_conf);
    } else {
      manager = std::make_unique<PassManager>(passes, json_conf);
    }
    manager->set_testing_mode();
    ConfigFiles conf(json_conf);
    manager->run_passes(stores, conf);
  }

  virtual ~RedexIntegrationTest() {}
};

/*
 * We compare IRCode objects by serializing them first. However, the serialized
 * forms lack newlines between instructions and so are rather difficult to read.
 * It's nice to print the original IRCode objects which have those newlines.
 *
 * This is a macro instead of a function so that the error messages will contain
 * the right line numbers.
 */
#define EXPECT_CODE_EQ(a, b)                                          \
  do {                                                                \
    IRCode* aCode = a;                                                \
    IRCode* bCode = b;                                                \
    std::string aStr = assembler::to_string(aCode);                   \
    std::string bStr = assembler::to_string(bCode);                   \
    if (aStr == bStr) {                                               \
      SUCCEED();                                                      \
    } else {                                                          \
      auto p = std::mismatch(aStr.begin(), aStr.end(), bStr.begin()); \
      FAIL() << '\n'                                                  \
             << "S-expressions failed to match: \n"                   \
             << aStr << '\n'                                          \
             << bStr << '\n'                                          \
             << std::string(p.first - aStr.begin(), '.') + "^\n"      \
             << "\nExpected:\n"                                       \
             << show(aCode) << "\nto be equal to:\n"                  \
             << show(bCode);                                          \
    }                                                                 \
  } while (0);
