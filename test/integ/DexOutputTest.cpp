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
    code_items.reserve(dout.m_code_item_emits.size());
    for (auto& ci : dout.m_code_item_emits) {
      code_items.push_back(ci.method);
    }
    return code_items;
  }
};

TEST_F(DexOutputTest, TEST_SIMILARITY_ORDERER) {
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
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

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

TEST_F(DexOutputTest, TEST_SIMILARITY_ORDERER_PERF_SENSITIVE) {
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

  (*classes)[1]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);
  (*classes)[2]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);

  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

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

TEST_F(DexOutputTest, TEST_COMPRESSION_ORDERER) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_PROFILED_ORDER,
                                   SortMode::METHOD_SIMILARITY};

  Json::Value cfg;
  std::istringstream temp_json(
      "{\"method_similarity_order\":{\"use_compression_conscious_order\":true}"
      "}");
  temp_json >> cfg;

  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

  dout.prepare(SortMode::DEFAULT, sort_modes, config_files, "dex\n039");
  auto code_items = get_ordered_methods(dout);

  uint32_t i = 0;
  std::vector<std::string> method_names(code_items.size());
  for (auto* m : code_items) {
    method_names[i++] = show(m);
  }

  std::vector<std::string> expected_order = {
      "LDexOutputTest$NonPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$PerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$SecondPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest;.DgetSixpublic:()I",
      "LDexOutputTest;.AjustReturnFive:()I",
      "LDexOutputTest;.EjustReturnFive:()I",
      "LDexOutputTest;.BjustCallSixpublic:()I",
      "LDexOutputTest;.GjustCallSixpublic:()I",
      "LDexOutputTest$NonPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$PerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$SecondPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest;.CsomeLogic:(I)I",
      "LDexOutputTest;.FsomeLogic:(I)I",
      "LDexOutputTest;.HsomeLogic:(I)I",
      "LDexOutputTest$NonPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$PerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$SecondPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest;.<init>:()V"};

  EXPECT_TRUE(std::equal(method_names.begin(), method_names.end(),
                         expected_order.begin()));
}

TEST_F(DexOutputTest, TEST_COMPRESSION_ORDERER_PERF_SENSITIVE) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_PROFILED_ORDER,
                                   SortMode::METHOD_SIMILARITY};

  Json::Value cfg;
  std::istringstream temp_json(
      "{\"method_similarity_order\":{\"use_class_level_perf_sensitivity\":true,"
      "\"use_compression_conscious_order\":true}"
      "}");
  temp_json >> cfg;

  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  (*classes)[1]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);
  (*classes)[2]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);

  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

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
      "LDexOutputTest$NonPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest;.CsomeLogic:(I)I",
      "LDexOutputTest;.FsomeLogic:(I)I",
      "LDexOutputTest;.HsomeLogic:(I)I",
      "LDexOutputTest$NonPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest;.<init>:()V",
      "LDexOutputTest$NonPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest;.DgetSixpublic:()I",
      "LDexOutputTest;.AjustReturnFive:()I",
      "LDexOutputTest;.EjustReturnFive:()I",
      "LDexOutputTest;.BjustCallSixpublic:()I",
      "LDexOutputTest;.GjustCallSixpublic:()I"};

  EXPECT_TRUE(std::equal(method_names.begin(), method_names.end(),
                         expected_order.begin()));
}

TEST_F(DexOutputTest, TEST_COLDSTART_ORDER) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_COLDSTART_ORDER,
                                   SortMode::METHOD_SIMILARITY};
  std::string profile_path = std::getenv("coldstart_methods_file");

  Json::Value cfg;
  std::istringstream temp_json("{\"coldstart_methods_file\":\"" + profile_path +
                               "\"}");
  temp_json >> cfg;

  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  (*classes)[1]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);
  (*classes)[2]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);

  DexMethod::make_method("LDexOutputTest2$Class;.someRandomMethodNotInDex:(I)I")
      ->make_concrete(ACC_PUBLIC, false);

  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

  dout.prepare(SortMode::DEFAULT, sort_modes, config_files, "dex\n039");
  auto code_items = get_ordered_methods(dout);

  uint32_t i = 0;
  std::vector<std::string> method_names(code_items.size());
  for (auto* m : code_items) {
    method_names[i++] = show(m);
  }

  std::vector<std::string> expected_order = {
      "LDexOutputTest$PerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$SecondPerfSensitiveClass;.<init>:(LDexOutputTest;)V",
      "LDexOutputTest$PerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$SecondPerfSensitiveClass;.EjustReturnFive:()I",
      "LDexOutputTest$SecondPerfSensitiveClass;.FsomeLogic:(I)I",
      "LDexOutputTest$PerfSensitiveClass;.FsomeLogic:(I)I",
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

TEST_F(DexOutputTest, TEST_COLDSTART_ORDER_EMPTY_FILE) {
  std::vector<SortMode> sort_modes{SortMode::METHOD_COLDSTART_ORDER,
                                   SortMode::METHOD_SIMILARITY};
  Json::Value cfg;
  ConfigFiles config_files(cfg);
  config_files.parse_global_config();
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  (*classes)[1]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);
  (*classes)[2]->set_perf_sensitive(PerfSensitiveGroup::BETAMAP_ORDERED);

  auto scope = build_class_scope(stores);

  // Lower the code
  walk::parallel::methods<>(
      scope, [](DexMethod* m) { instruction_lowering::lower(m, true); });

  auto gtypes = std::make_shared<GatheredTypes>(&*classes);
  DexOutput dout("", &*classes, std::move(gtypes), true, 0, nullptr, 0,
                 DebugInfoKind::NoCustomSymbolication, nullptr, config_files,
                 pos_mapper.get(), nullptr, nullptr, DexOutputConfig(), 25);

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
