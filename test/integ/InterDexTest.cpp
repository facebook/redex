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
#include "DexLimits.h"
#include "DexLoader.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"

#include "InterDexPass.h"

class InterDexTest : public RedexIntegrationTest {
 public:
  void define_test(const std::vector<std::string>& betmap,
                   const std::string& expected_manifest,
                   bool minimize_cross_dex_refs_explore_alternatives = false,
                   bool order_interdex = true,
                   bool define_dynamically_dead_classes = false) {
    if (define_dynamically_dead_classes) {
      for (auto& cls : *classes) {
        if (cls->get_name() ==
            DexString::make_string("Lcom/facebook/redextest/C7;")) {
          cls->set_dynamically_dead();
        }
      }
    }

    std::cout << "Loaded classes: " << classes->size() << "\n";

    auto tmp_dir = redex::make_tmp_dir("redex_interdex_test_%%%%%%%%");

    auto betmap_file = make_betmap_file(tmp_dir.path, betmap);
    auto* config_file_env = std::getenv("config_file");
    always_assert_log(config_file_env,
                      "Config file must be specified to InterDexTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;
    cfg["apk_dir"] = tmp_dir.path;
    cfg["coldstart_classes"] = betmap_file;
    if (minimize_cross_dex_refs_explore_alternatives) {
      cfg["InterDexPass"]["minimize_cross_dex_refs"] = true;
      cfg["InterDexPass"]["reorder_dynamically_dead_classes"] = true;
      cfg["InterDexPass"]["reserved_trefs"] = kOldMaxTypeRefs - 16;
      cfg["InterDexPass"]["minimize_cross_dex_refs_explore_alternatives"] = 24;
      cfg["InterDexPass"]["order_interdex"] = order_interdex;
    }

    auto path = boost::filesystem::path(tmp_dir.path);
    path += boost::filesystem::path::preferred_separator;
    path += "assets";
    path += boost::filesystem::path::preferred_separator;
    path += "secondary-program-dex-jars";
    boost::filesystem::create_directories(path);

    Pass* pass = nullptr;
    pass = new interdex::InterDexPass(/* register_plugins = */ false);
    std::vector<Pass*> passes = {pass};

    run_passes(passes, nullptr, cfg);

    std::ifstream manifest_in(path.string() + "/dex_manifest.txt");
    std::stringstream buffer;
    buffer << manifest_in.rdbuf();

    EXPECT_EQ(expected_manifest, buffer.str());
  }

  void define_throwing_test(
      const std::vector<std::string>& betmap,
      const std::string& expected_manifest,
      bool minimize_cross_dex_refs_explore_alternatives = false) {
    EXPECT_THROW(
        try {
          define_test(betmap,
                      expected_manifest,
                      minimize_cross_dex_refs_explore_alternatives);
        } catch (RedexException& e) {
          EXPECT_EQ(e.type, RedexError::INVALID_BETAMAP);
          throw;
        },
        RedexException);
  }

  std::string make_betmap_file(const std::string& tmp,
                               const std::vector<std::string>& betamap) {
    std::ofstream betamap_out;
    std::string path = tmp + "/classes.txt";
    betamap_out.open(path.c_str(), std::ios::out);
    for (const std::string& cls : betamap) {
      betamap_out << cls;
      betamap_out << "\n";
    }
    return path;
  }
};

/* clang-format off */

TEST_F(InterDexTest, interdex_noscroll_nobg_noext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
  );
}

TEST_F(InterDexTest, interdex_noscroll_nobg_ext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class",
      "com/facebook/redextest/C10.class",
      "com/facebook/redextest/C11.class",
      "com/facebook/redextest/C12.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
  );
}

TEST_F(InterDexTest, interdex_noscroll_bg_noext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "BackgroundSetStart0.class",
      "com/facebook/redextest/C5.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class",
      "com/facebook/redextest/C10.class",
      "com/facebook/redextest/C11.class",
      "com/facebook/redextest/C12.class",
      "BackgroundSetEnd0.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=1\n"
  );
}

TEST_F(InterDexTest, interdex_noscroll_bg_ext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "BackgroundSetStart0.class",
      "com/facebook/redextest/C5.class",
      "com/facebook/redextest/C6.class",
      "BackgroundSetEnd0.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=1,primary=0,scroll=0,background=1\n"
  );
}

TEST_F(InterDexTest, interdex_scroll_nobg_noext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "ScrollSetStart0.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "ScrollSetEnd0.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=1,background=0\n"
  );
}

TEST_F(InterDexTest, interdex_scroll_nobg_ext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "ScrollSetStart0.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "ScrollSetEnd0.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class",
      "com/facebook/redextest/C10.class",
      "com/facebook/redextest/C11.class",
      "com/facebook/redextest/C12.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=1,background=0\n"
  );
}

TEST_F(InterDexTest, interdex_scroll_bg_noext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "ScrollSetStart0.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "ScrollSetEnd0.class",
      "BackgroundSetStart0.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class",
      "com/facebook/redextest/C10.class",
      "com/facebook/redextest/C11.class",
      "com/facebook/redextest/C12.class",
      "BackgroundSetEnd0.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=1,background=1\n"
  );
}

TEST_F(InterDexTest, interdex_scroll_bg_ext) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "com/facebook/redextest/C2.class",
      "DexEndMarker1.class",
      "ScrollSetStart0.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "ScrollSetEnd0.class",
      "BackgroundSetStart0.class",
      "com/facebook/redextest/C6.class",
      "BackgroundSetEnd0.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=1,primary=0,scroll=1,background=1\n"
  );
}

TEST_F(InterDexTest, interdex_cross_dex_ref_minimization) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "DexEndMarker0.class",
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=0\n",
    /* minimize_cross_dex_refs_explore_alternatives */ true
  );

  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 3);
  EXPECT_EQ(stores[0].get_dexen()[0].size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[1].size(), 12);
  EXPECT_EQ(stores[0].get_dexen()[2].size(), 4);

  // First regular class is the one with highest seed weight
  EXPECT_EQ(stores[0].get_dexen()[1].front()->get_name()->str(), "Lcom/facebook/redextest/C7;");
}

TEST_F(InterDexTest, interdex_dynamically_dead) {
  define_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "DexEndMarker0.class",
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex03/Canary;,ordinal=3,coldstart=0,extended=0,primary=0,scroll=0,background=0\n",
    /* minimize_cross_dex_refs_explore_alternatives */ true, /* order_interdex*/ true, /* define_dynamically_dead_classes */true
  );

  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 4);
  EXPECT_EQ(stores[0].get_dexen()[0].size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[1].size(), 12);
  EXPECT_EQ(stores[0].get_dexen()[2].size(), 3);
  EXPECT_EQ(stores[0].get_dexen()[3].size(), 2);

  // The dynamically_dead class should be in the last seconday dex.
  EXPECT_EQ(stores[0].get_dexen()[3].front()->get_name()->str(), "Lcom/facebook/redextest/C7;");
}

TEST_F(InterDexTest, interdex_test_validate_class_spec) {
  define_throwing_test({
      "com/facebook/redextest/InterDexPrimary.class",
      "com/facebook/redextest/C0.class",
      "DexEndMarker0.class",
      "com/facebook/redextest/C1.class",
      "Lcom/facebook/redextest/C2;", // bad
      "DexEndMarker1.class",
      "com/facebook/redextest/C3.class",
      "com/facebook/redextest/C4.class",
      "com/facebook/redextest/C5.class",
      "com/facebook/redextest/C6.class",
      "com/facebook/redextest/C7.class",
      "com/facebook/redextest/C8.class",
      "com/facebook/redextest/C9.class"
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=1,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex02/Canary;,ordinal=2,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
  );
}

TEST_F(InterDexTest, without_order_interdex) {
  // When order_interdex is off, classes are distributed across dexes in a way that's not guided by the betamap; however, within each dexes, betamap classes are still ordered according to the betamap.
  define_test({
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
      "com/facebook/redextest/C9.class",
      "com/facebook/redextest/C10.class",
      "com/facebook/redextest/C11.class",
      "com/facebook/redextest/C12.class",
      "DexEndMarker0.class",
    },
    "Lsecondary/dex00/Canary;,ordinal=0,coldstart=0,extended=0,primary=0,scroll=0,background=0\n"
    "Lsecondary/dex01/Canary;,ordinal=1,coldstart=0,extended=0,primary=0,scroll=0,background=0\n",
    /* minimize_cross_dex_refs_explore_alternatives */ true,
    /* order_interdex */ false
  );

  auto get_class = [&](size_t dex_idx, size_t idx) {
    return stores[0].get_dexen()[dex_idx][idx]->get_name()->str();
  };
  EXPECT_EQ(stores.size(), 1);
  EXPECT_EQ(stores[0].get_dexen().size(), 2);
  EXPECT_EQ(stores[0].get_dexen()[0].size(), 11);
  EXPECT_EQ(get_class(0, 0), "Lcom/facebook/redextest/InterDexPrimary;");
  EXPECT_EQ(get_class(0, 1), "Lcom/facebook/redextest/C0;");
  EXPECT_EQ(get_class(0, 2), "Lcom/facebook/redextest/C1;");
  EXPECT_EQ(get_class(0, 3), "Lcom/facebook/redextest/C2;");
  EXPECT_EQ(get_class(0, 4), "Lcom/facebook/redextest/C3;");
  EXPECT_EQ(get_class(0, 5), "Lcom/facebook/redextest/C4;");
  EXPECT_EQ(get_class(0, 6), "Lcom/facebook/redextest/C7;");
  EXPECT_EQ(get_class(0, 7), "Lcom/facebook/redextest/C10;");
  EXPECT_EQ(get_class(0, 8), "Lcom/facebook/redextest/C11;");
  EXPECT_EQ(get_class(0, 9), "Lcom/facebook/redextest/C12;");
  EXPECT_EQ(stores[0].get_dexen()[1].size(), 6);
  EXPECT_EQ(get_class(1, 0), "Lcom/facebook/redextest/C5;");
  EXPECT_EQ(get_class(1, 1), "Lcom/facebook/redextest/C6;");
  EXPECT_EQ(get_class(1, 2), "Lcom/facebook/redextest/C8;");
  EXPECT_EQ(get_class(1, 3), "Lcom/facebook/redextest/C9;");

  // First regular class is the one with highest seed weight
}

/* clang-format on */
