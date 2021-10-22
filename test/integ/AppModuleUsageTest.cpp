/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <fstream>
#include <json/json.h>

#include "AppModuleUsage.h"
#include "DexAccess.h"
#include "RedexTest.h"

namespace {
void split_two_stores(std::vector<DexStore>& stores) {
  auto& root_store = stores.at(0);
  DexMetadata second_dex_metadata;
  second_dex_metadata.set_id("AppModule");
  auto& root_dex_classes = root_store.get_dexen().at(0);
  // add second store with OtherClass & ThirdClass
  DexStore second_store(second_dex_metadata);
  auto other_class =
      type_class(DexType::get_type("LAppModuleUsageOtherClass;"));
  auto third_class =
      type_class(DexType::get_type("LAppModuleUsageThirdClass;"));
  second_store.add_classes(std::vector<DexClass*>{other_class});
  second_store.add_classes(std::vector<DexClass*>{third_class});
  stores.emplace_back(second_store);
  // remove OtherClass & ThirdClass from root store classes
  root_dex_classes.erase(
      std::find(root_dex_classes.begin(), root_dex_classes.end(), other_class));
  root_dex_classes.erase(
      std::find(root_dex_classes.begin(), root_dex_classes.end(), third_class));
}
void split_three_stores(std::vector<DexStore>& stores) {
  auto& root_store = stores.at(0);
  DexMetadata second_dex_metadata;
  second_dex_metadata.set_id("AppModule");
  DexMetadata third_dex_metadata;
  third_dex_metadata.set_id("OtherModule");
  auto& root_dex_classes = root_store.get_dexen().at(0);
  // add second store with OtherClass
  DexStore second_store(second_dex_metadata);
  auto other_class =
      type_class(DexType::get_type("LAppModuleUsageOtherClass;"));
  second_store.add_classes(std::vector<DexClass*>{other_class});
  stores.emplace_back(second_store);
  // add third store with ThirdClass
  DexStore third_store(third_dex_metadata);
  auto third_class =
      type_class(DexType::get_type("LAppModuleUsageThirdClass;"));
  third_store.add_classes(std::vector<DexClass*>{third_class});
  stores.emplace_back(third_store);
  // remove OtherClass & ThirdClass from root store classes
  root_dex_classes.erase(
      std::find(root_dex_classes.begin(), root_dex_classes.end(), other_class));
  root_dex_classes.erase(
      std::find(root_dex_classes.begin(), root_dex_classes.end(), third_class));
}
} // namespace

struct AppModuleUsageTest : public RedexIntegrationTest {};

TEST_F(AppModuleUsageTest, testOneStore) {
  // AppModuleUsageClass and AppModuleUsageOtherClass are in the root store
  auto config_file_env = std::getenv("default_config_file");
  std::ifstream config_file(config_file_env, std::ifstream::binary);
  run_passes({new AppModuleUsagePass});
  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at(
                "num_methods_access_app_module"),
            0);
  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at("num_violations"), 0);
}

TEST_F(AppModuleUsageTest, testTwoStores) {
  split_two_stores(stores);

  // root_store holds AppModuleUsageClass
  // second_store holds AppModuleUsageOtherClass & AppModuleUsageThirdClass
  // configure to not crash on violations
  auto config_file_env = std::getenv("config_file");
  always_assert_log(config_file_env,
                    "Config file must be specified to AppModuleUsageTest.\n");

  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;
  run_passes({new AppModuleUsagePass}, nullptr, cfg);

  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at(
                "num_methods_access_app_module"),
            9);
  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at("num_violations"), 5);
}

TEST_F(AppModuleUsageTest, testTwoStoresCrash) {
  split_two_stores(stores);
  auto config_file_env = std::getenv("default_config_file");
  std::ifstream config_file(config_file_env, std::ifstream::binary);

  // root_store holds AppModuleUsageClass
  // second_store holds AppModuleUsageOtherClass & AppModuleUsageThirdClass
  // will crash on violation without config
  EXPECT_ANY_THROW(run_passes({new AppModuleUsagePass}));
}

TEST_F(AppModuleUsageTest, testThreeStores) {
  split_three_stores(stores);

  // root_store holds AppModuleUsageClass
  // second_store holds AppModuleUsageOtherClass
  // third_store holds AppModuleUsageThirdClass
  // configure to not crash on violations
  auto config_file_env = std::getenv("config_file");
  always_assert_log(config_file_env,
                    "Config file must be specified to AppModuleUsageTest.\n");

  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;
  run_passes({new AppModuleUsagePass}, nullptr, cfg);

  // AppModuleUsageOtherClass and AppModuleUsageThirdClass each have a method
  // with a App module access when in different stores
  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at(
                "num_methods_access_app_module"),
            11);
  // 2 extra violations in AppModuleUsageOtherClass when
  // AppMopuleUsageThirdClass is in another store
  EXPECT_EQ(pass_manager->get_pass_info()[0].metrics.at("num_violations"), 7);
}

TEST_F(AppModuleUsageTest, testThreeStoresCrash) {
  split_three_stores(stores);
  auto config_file_env = std::getenv("default_config_file");
  std::ifstream config_file(config_file_env, std::ifstream::binary);

  // root_store holds AppModuleUsageClass
  // second_store holds AppModuleUsageOtherClass
  // third_store holds AppModuleUsageThirdClass
  // will crash on violation without config
  EXPECT_ANY_THROW(run_passes({new AppModuleUsagePass}));
}
