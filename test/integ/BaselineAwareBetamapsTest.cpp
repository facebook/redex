/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLimits.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"

#include "InterDexPass.h"
#include <boost/filesystem.hpp>

class BaselineAwareBetamapsTest : public RedexIntegrationTest {
 public:
  void define_test(const std::vector<std::string>& betamap,
                   const std::string& method_profile_path) {
    auto tmp_dir = redex::make_tmp_dir("redex_bab_test_%%%%%%%%");

    auto betamap_file = make_betamap_file(tmp_dir.path, betamap);
    auto config_file_env = std::getenv("config_file");
    always_assert_log(
        config_file_env,
        "Config file must be specified to BaselineAwareBetamapsTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;
    cfg["apk_dir"] = tmp_dir.path;
    cfg["coldstart_classes"] = betamap_file;

    Json::Value mp_val = Json::arrayValue;
    mp_val.resize(1);
    mp_val[0] = method_profile_path;
    cfg["agg_method_stats_files"] = mp_val;

    Pass* pass = nullptr;
    pass = new interdex::InterDexPass(/* register_plugins = */ false);
    std::vector<Pass*> passes = {pass};

    run_passes(passes, nullptr, cfg);
  }

  void define_throwing_test(const std::vector<std::string>& betamap,
                            const std::string& expected_manifest) {
    EXPECT_THROW(
        try {
          define_test(betamap, expected_manifest);
        } catch (RedexException& e) {
          EXPECT_EQ(e.type, RedexError::INVALID_BETAMAP);
          throw;
        },
        RedexException);
  }

  std::string make_betamap_file(const std::string& tmp,
                                const std::vector<std::string>& betamap) {
    std::ofstream betamap_out;
    std::string path = tmp + "/classes.txt";
    betamap_out.open(path.c_str(), std::ios::out);
    for (const std::string& cls : betamap) {
      betamap_out << cls;
      betamap_out << std::endl;
    }
    return path;
  }
};

TEST_F(BaselineAwareBetamapsTest, test1) {
  auto method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";
  define_test(
      {
          "com/facebook/redextest/InterDexPrimary.class",
          "com/facebook/redextest/C0.class",
          "com/facebook/redextest/C1.class",
          "com/facebook/redextest/C2.class",
          "com/facebook/redextest/C3.class",
          "com/facebook/redextest/C4.class",
          "com/facebook/redextest/C5.class",
          "com/facebook/redextest/C6.class",
          "ColdStart20PctEnd.class",
          "com/facebook/redextest/C9.class",
          "com/facebook/redextest/C10.class",
          "com/facebook/redextest/C11.class",
          "com/facebook/redextest/C12.class",
          "ColdStart1PctEnd.class",
          "DexEndMarker0.class",
          "com/facebook/redextest/C7.class",
          "com/facebook/redextest/C8.class",
      },
      method_profile_path);

  auto get_class = [&](size_t dex_idx, size_t idx) {
    return stores[0].get_dexen()[dex_idx][idx]->get_name()->str();
  };
  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[0].size(), 3);
  EXPECT_EQ(get_class(0, 0), "Lcom/facebook/redextest/C10;");
  EXPECT_EQ(get_class(0, 1), "Lcom/facebook/redextest/C11;");
  EXPECT_EQ(get_class(0, 2), "Lsecondary/dex00/Canary;");

  EXPECT_EQ(stores[0].get_dexen()[1].size(), 14);

  EXPECT_EQ(get_class(1, 0), "Lcom/facebook/redextest/C7;");
  EXPECT_EQ(get_class(1, 1), "Lcom/facebook/redextest/C8;");

  EXPECT_EQ(get_class(1, 2), "Lcom/facebook/redextest/InterDexPrimary;");

  EXPECT_EQ(get_class(1, 3), "Lcom/facebook/redextest/C0;");
  EXPECT_EQ(get_class(1, 4), "Lcom/facebook/redextest/C12;");
  EXPECT_EQ(get_class(1, 5), "Lcom/facebook/redextest/C1;");
  EXPECT_EQ(get_class(1, 6), "Lcom/facebook/redextest/C2;");
  EXPECT_EQ(get_class(1, 7), "Lcom/facebook/redextest/C3;");
  EXPECT_EQ(get_class(1, 8), "Lcom/facebook/redextest/C4;");
  EXPECT_EQ(get_class(1, 9), "Lcom/facebook/redextest/C5;");
  EXPECT_EQ(get_class(1, 10), "Lcom/facebook/redextest/C6;");
  EXPECT_EQ(get_class(1, 11), "Lcom/facebook/redextest/C9;");
  EXPECT_EQ(get_class(1, 12), "Lcom/facebook/redextest/InterDexSecondary;");
  EXPECT_EQ(get_class(1, 13), "Lsecondary/dex01/Canary;");
}

TEST_F(BaselineAwareBetamapsTest, test2) {
  auto method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";

  define_test(
      {
          "com/facebook/redextest/InterDexPrimary.class",
          "com/facebook/redextest/C0.class",
          "com/facebook/redextest/C1.class",
          "com/facebook/redextest/C2.class",
          "com/facebook/redextest/C3.class",
          "com/facebook/redextest/C4.class",
          "com/facebook/redextest/C5.class",
          "com/facebook/redextest/C6.class",
          "com/facebook/redextest/C7.class",
          "com/facebook/redextest/C8.class",
          "ColdStart20PctEnd.class",
          "DexEndMarker0.class",
          "com/facebook/redextest/C9.class",
          "com/facebook/redextest/C10.class",
          "com/facebook/redextest/C11.class",
          "com/facebook/redextest/C12.class",
          "ColdStart1PctEnd.class",
          "DexEndMarker1.class",
      },
      method_profile_path);

  auto get_class = [&](size_t dex_idx, size_t idx) {
    return stores[0].get_dexen()[dex_idx][idx]->get_name()->str();
  };
  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 2);

  EXPECT_EQ(stores[0].get_dexen()[0].size(), 3);
  EXPECT_EQ(get_class(0, 0), "Lcom/facebook/redextest/C10;");
  EXPECT_EQ(get_class(0, 1), "Lcom/facebook/redextest/C11;");
  EXPECT_EQ(get_class(0, 2), "Lsecondary/dex00/Canary;");

  EXPECT_EQ(stores[0].get_dexen()[1].size(), 14);
  EXPECT_EQ(get_class(1, 0), "Lcom/facebook/redextest/InterDexPrimary;");

  EXPECT_EQ(get_class(1, 1), "Lcom/facebook/redextest/C0;");
  EXPECT_EQ(get_class(1, 2), "Lcom/facebook/redextest/C12;");
  EXPECT_EQ(get_class(1, 3), "Lcom/facebook/redextest/C1;");
  EXPECT_EQ(get_class(1, 4), "Lcom/facebook/redextest/C2;");
  EXPECT_EQ(get_class(1, 5), "Lcom/facebook/redextest/C3;");
  EXPECT_EQ(get_class(1, 6), "Lcom/facebook/redextest/C4;");
  EXPECT_EQ(get_class(1, 7), "Lcom/facebook/redextest/C5;");
  EXPECT_EQ(get_class(1, 8), "Lcom/facebook/redextest/C6;");
  EXPECT_EQ(get_class(1, 9), "Lcom/facebook/redextest/C8;");
  EXPECT_EQ(get_class(1, 10), "Lcom/facebook/redextest/C9;");
  EXPECT_EQ(get_class(1, 11), "Lcom/facebook/redextest/InterDexSecondary;");
  EXPECT_EQ(get_class(1, 12), "Lcom/facebook/redextest/C7;");
  EXPECT_EQ(get_class(1, 13), "Lsecondary/dex01/Canary;");
}

/* clang-format on */
