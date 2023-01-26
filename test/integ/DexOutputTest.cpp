/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <json/json.h>

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IODIMetadata.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"
#include "Show.h"
#include "Walkers.h"

#include "DexOutput.h"

class DexOutputTest : public RedexIntegrationTest {
 public:
  DexOutputTest() {}

  std::vector<DexMethod*> get_ordered_methods(DexOutput& dout) {
    std::vector<DexMethod*> code_items;
    for (auto& ci : dout.m_code_item_emits) {
      code_items.push_back(ci.method);
    }
    return code_items;
  }
};

TEST_F(DexOutputTest, testProfiledSecondaryOrder) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_PROFILED_SECONDARY_ORDER};

  Json::Value config(Json::objectValue);
  config["bytecode_sort_mode"] = Json::arrayValue;
  config["bytecode_sort_mode"].append("method_profiled_order");
  config["bytecode_sort_mode"].append("method_profiled_secondary_order");

  auto profile_path = std::getenv("SECONDARY_PROFILE");
  config["secondary_method_stats_files"] = Json::arrayValue;
  config["secondary_method_stats_files"].append(profile_path);

  ConfigFiles config_files(config);
  config_files.parse_global_config();

  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), nullptr, true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), nullptr,
                 25);

  dout.prepare(SortMode::DEFAULT, sort_modes, config_files, "dex\n039");
  auto code_items = get_ordered_methods(dout);

  uint32_t i = 0;
  std::vector<std::string> method_names(code_items.size());
  for (auto* m : code_items) {
    method_names[i++] = show(m);
  }

  // These methods have appear100 == 0.0
  std::set<std::string> expected_end_bucket;
  expected_end_bucket.insert("LDexOutputTest;.CsomeLogic:(I)I");
  expected_end_bucket.insert("LDexOutputTest;.FsomeLogic:(I)I");
  expected_end_bucket.insert("LDexOutputTest;.HsomeLogic:(I)I");
  expected_end_bucket.insert("LDexOutputTest;.BjustCallSixpublic:()I");
  expected_end_bucket.insert("LDexOutputTest;.GjustCallSixpublic:()I");

  // Corresponding Java file used to produce test dex has 18 methods.
  EXPECT_EQ(method_names.size(), 18);

  for (i = 0; i < 5; ++i) {
    int idx = method_names.size() - 1 - i;
    EXPECT_EQ(expected_end_bucket.count(method_names.at(idx)), 1)
        << "Expected end bucket did not contain " << method_names.at(idx)
        << " at index " << i;
  }
}

TEST_F(DexOutputTest, testSimilarityOrderer) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_PROFILED_ORDER,
                                   SortMode::METHOD_SIMILARITY};

  Json::Value cfg;
  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), nullptr, true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), nullptr,
                 25);

  dout.prepare(SortMode::DEFAULT, sort_modes, config_files, "dex\n039");
  auto code_items = get_ordered_methods(dout);

  uint32_t i = 0;
  std::vector<std::string> method_names(code_items.size());
  for (auto* m : code_items) {
    method_names[i++] = show(m);
  }

  std::vector<std::string> expected_order = {
      "LDexOutputTest$NonPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$PerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$SecondPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest;.<init>:()V",
      "LDexOutputTest$NonPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$PerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$SecondPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest;.AjustReturnFive:()I",
      "LDexOutputTest;.EjustReturnFive:()I",
      "LDexOutputTest;.DgetSixpublic:()I",
      "LDexOutputTest$NonPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$PerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$SecondPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest;.CsomeLogic:(I)I",
      "LDexOutputTest;.FsomeLogic:(I)I",
      "LDexOutputTest;.HsomeLogic:(I)I",
      "LDexOutputTest;.BjustCallSixpublic:()I",
      "LDexOutputTest;.GjustCallSixpublic:()I"};

  EXPECT_TRUE(std::equal(method_names.begin(), method_names.end(),
                         expected_order.begin()));
}

TEST_F(DexOutputTest, testPerfSensitive) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_PROFILED_ORDER,
                                   SortMode::METHOD_SIMILARITY};

  Json::Value cfg;
  std::istringstream temp_json(
      "{\"method_similarity_order\":{\"use_class_level_perf_sensitivity\":true}"
      "}");
  temp_json >> cfg;

  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  (*classes)[1]->set_perf_sensitive(true);
  (*classes)[2]->set_perf_sensitive(true);

  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), nullptr, true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig{}, nullptr,
                 25);

  dout.prepare(SortMode::DEFAULT, sort_modes, config_files, "dex\n039");
  auto code_items = get_ordered_methods(dout);

  uint32_t i = 0;
  std::vector<std::string> method_names(code_items.size());
  for (auto* m : code_items) {
    method_names[i++] = show(m);
  }

  std::vector<std::string> expected_order = {
      "LDexOutputTest$PerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$PerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$PerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$SecondPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$SecondPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$SecondPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$NonPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest;.<init>:()V",
      "LDexOutputTest$NonPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest;.AjustReturnFive:()I",
      "LDexOutputTest;.EjustReturnFive:()I",
      "LDexOutputTest;.DgetSixpublic:()I",
      "LDexOutputTest$NonPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest;.CsomeLogic:(I)I",
      "LDexOutputTest;.FsomeLogic:(I)I",
      "LDexOutputTest;.HsomeLogic:(I)I",
      "LDexOutputTest;.BjustCallSixpublic:()I",
      "LDexOutputTest;.GjustCallSixpublic:()I"};

  EXPECT_TRUE(std::equal(method_names.begin(), method_names.end(),
                         expected_order.begin()));
}
