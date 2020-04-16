/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"

#include "InterDexPass.h"
#include <boost/filesystem.hpp>

class InterDexTest : public RedexIntegrationTest {
 public:
  void define_test(const std::vector<std::string>& betmap,
                   const std::string& expected_manifest) {
    std::cout << "Loaded classes: " << classes->size() << std::endl;

    auto tmp_dir = make_tmp_dir();

    auto betmap_file = make_betmap_file(tmp_dir, betmap);
    auto config_file_env = std::getenv("config_file");
    always_assert_log(config_file_env,
                      "Config file must be specified to InterDexTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;
    cfg["apk_dir"] = tmp_dir;
    cfg["coldstart_classes"] = betmap_file;

    auto path = boost::filesystem::path(tmp_dir);
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

  std::string make_tmp_dir() {
    auto path = boost::filesystem::temp_directory_path();
    path += boost::filesystem::unique_path("/redex_interdex_test_%%%%%%%%");
    boost::filesystem::create_directories(path);
    return path.string();
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

/* clang-format on */
