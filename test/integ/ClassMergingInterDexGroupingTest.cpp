/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h>

#include "InterDexGrouping.h"
#include "RedexTest.h"

using namespace class_merging;

class ClassMergingInterDexGroupingTest : public RedexIntegrationTest {
 public:
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

  std::vector<ConstTypeHashSet> run_interdex_grouping(
      const std::vector<std::string>& betamap,
      const std::vector<std::string>& merging_targets) {
    auto tmp_dir = redex::make_tmp_dir("redex_test_%%%%%%%%");
    auto betamap_file = make_betamap_file(tmp_dir.path, betamap);

    Json::Value cfg;
    cfg["coldstart_classes"] = betamap_file;

    auto scope = build_class_scope(stores);
    ConfigFiles conf(cfg);
    conf.parse_global_config();

    InterDexGroupingConfig grouping_config(InterDexGroupingType::FULL);
    grouping_config.inferring_mode =
        InterDexGroupingInferringMode::kExactSymbolMatch;

    ConstTypeHashSet type_set;
    for (const auto& cls_name : merging_targets) {
      auto type = DexType::get_type(cls_name);
      if (type != nullptr) {
        type_set.insert(DexType::get_type(cls_name));
      }
    }
    InterDexGrouping idgrouping(scope, conf, grouping_config, type_set);
    return idgrouping.get_all_interdexing_groups();
  }
};

/* clang-format off */

TEST_F(ClassMergingInterDexGroupingTest, three_groups) {
  const auto groups = run_interdex_grouping(
      {"com/facebook/redextest/Base.class",
       "com/facebook/redextest/A.class",
       "DexEndMarker0.class",
       "com/facebook/redextest/B.class",
       "com/facebook/redextest/C.class",
       "com/facebook/redextest/D.class",
       "DexEndMarker1.class"},
      {"Lcom/facebook/redextest/Base;",
       "Lcom/facebook/redextest/A;",
       "Lcom/facebook/redextest/B;",
       "Lcom/facebook/redextest/C;",
       "Lcom/facebook/redextest/D;",
       "Lcom/facebook/redextest/E;",
       "Lcom/facebook/redextest/F;",
       "Lcom/facebook/redextest/G;",
       "Lcom/facebook/redextest/H;"});

  EXPECT_EQ(groups.size(), 3);
  EXPECT_EQ(groups[0].size(), 2);
  EXPECT_TRUE(
      groups[0].count(DexType::get_type("Lcom/facebook/redextest/Base;")));
  EXPECT_TRUE(groups[0].count(DexType::get_type("Lcom/facebook/redextest/A;")));
  EXPECT_EQ(groups[1].size(), 3);
  EXPECT_TRUE(groups[1].count(DexType::get_type("Lcom/facebook/redextest/B;")));
  EXPECT_TRUE(groups[1].count(DexType::get_type("Lcom/facebook/redextest/C;")));
  EXPECT_TRUE(groups[1].count(DexType::get_type("Lcom/facebook/redextest/D;")));
  EXPECT_EQ(groups[2].size(), 4);
  EXPECT_TRUE(groups[2].count(DexType::get_type("Lcom/facebook/redextest/E;")));
  EXPECT_TRUE(groups[2].count(DexType::get_type("Lcom/facebook/redextest/F;")));
  EXPECT_TRUE(groups[2].count(DexType::get_type("Lcom/facebook/redextest/G;")));
  EXPECT_TRUE(groups[2].count(DexType::get_type("Lcom/facebook/redextest/H;")));
}

/* clang-format on */
