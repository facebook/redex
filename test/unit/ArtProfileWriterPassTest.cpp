/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>
#include <json/value.h>

#include "ArtProfileWriterPass.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"

class ArtProfileWriterPassTest
    : public RedexTest,
      public ::testing::WithParamInterface<const char*> {};

TEST_P(ArtProfileWriterPassTest, abortsInsteadOfIgnoringFailedWrites) {
  ClassCreator creator(DexType::make_type("LProfileWrite;"));
  creator.set_super(type::java_lang_Object());
  creator.add_method(assembler::method_from_string(R"(
    (method (public static) "LProfileWrite;.m:()V" ((return-void)))
  )"));
  DexStore store("classes");
  store.add_classes({creator.create()});
  DexStoresVector stores;
  stores.emplace_back(std::move(store));
  Json::Value config;
  config["baseline_profile"]["default"]["use_final_redex_generated_profile"] =
      true;
  auto tmp = redex::make_tmp_dir("profile_write_failure%%%%%%%%");
  ConfigFiles conf(config);
  conf.set_outdir(tmp.path);
  conf.parse_global_config();
  std::filesystem::create_directory(conf.metafile(GetParam()));
  ArtProfileWriterPass pass;
  PassManager manager({&pass}, conf);
  EXPECT_THROW(manager.run_passes(stores, conf), RedexException);
}

INSTANTIATE_TEST_SUITE_P(
    ProfileFiles,
    ArtProfileWriterPassTest,
    ::testing::Values("default-baseline-profile.txt",
                      "additional-baseline-profiles.list"));
