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

#include "DexRemovalPass.h"
#include "InterDexPass.h"
#include <boost/filesystem.hpp>

class InterDexTest : public RedexIntegrationTest {
 public:
  void define_dex_removal_test(
      const std::vector<std::string>& betmap,
      bool minimize_cross_dex_refs_explore_alternatives = false) {
    std::cout << "Loaded classes: " << classes->size() << std::endl;

    auto tmp_dir = redex::make_tmp_dir("redex_interdex_test_%%%%%%%%");

    auto betmap_file = make_betmap_file(tmp_dir.path, betmap);
    auto config_file_env = std::getenv("config_file");
    always_assert_log(config_file_env,
                      "Config file must be specified to InterDexTest.\n");
    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;
    cfg["apk_dir"] = tmp_dir.path;
    cfg["coldstart_classes"] = betmap_file;

    if (minimize_cross_dex_refs_explore_alternatives) {
      cfg["InterDexPass"]["minimize_cross_dex_refs"] = true;
      cfg["InterDexPass"]["reserved_trefs"] = kOldMaxTypeRefs - 16;
      cfg["InterDexPass"]["minimize_cross_dex_refs_explore_alternatives"] = 24;
      cfg["InterDexPass"]["order_interdex"] = true;
    }

    auto path = boost::filesystem::path(tmp_dir.path);
    path += boost::filesystem::path::preferred_separator;
    path += "assets";
    path += boost::filesystem::path::preferred_separator;
    path += "secondary-program-dex-jars";
    boost::filesystem::create_directories(path);

    Pass* pass1 = nullptr;
    Pass* pass2 = nullptr;
    pass1 = new interdex::InterDexPass(/* register_plugins = */ false);
    pass2 = new DexRemovalPass();
    std::vector<Pass*> passes = {pass1, pass2};
    run_passes(passes, nullptr, cfg);
  }

  std::string make_betmap_file(const std::string& tmp,
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

/* clang-format off */

// This test run DexRemovalPass after InterDexPass, and the dexes are the same as test interdex_cross_dex_ref_minimization. After InterDexPass, the dexes are:
//  stores.size() == 1
//  stores[0].get_dexen().size() ==  3
//  stores[0].get_dexen()[0].size() ==  2
//  stores[0].get_dexen()[1].size() == 12;
//  stores[0].get_dexen()[2].size() ==  4;
// Then DexRemovalPass will move all classe from dex 2 to dex 1 and remove dex 2.
TEST_F(InterDexTest, dex_removal) {
  define_dex_removal_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "DexEndMarker0.class",
    },
    /* minimize_cross_dex_refs_explore_alternatives */ true
    );

  for (size_t i = 0 ; i < stores[0].get_dexen().size(); i++) {
    std::cout << "in dex " << i << std::endl;
    for (size_t j = 0; j < stores[0].get_dexen()[i].size(); j++) {
      std::cout << "  " << stores[0].get_dexen()[i][j]->get_name()->str() << std::endl;
    }
  }

  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[0].size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[1].size(), 15);
}
/* clang-format on */
