/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InjectDebug.h"

#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "InstructionLowering.h"
#include "RedexContext.h"
#include "ToolsCommon.h"

InjectDebug::InjectDebug(const std::string& outdir,
                         const std::vector<std::string>& dex_files)
    : m_conf(Json::Value(), outdir), m_dex_files(dex_files) {
  if (!g_redex) {
    g_redex = new RedexContext();
  }
}

InjectDebug::~InjectDebug() {
  delete g_redex;
  g_redex = nullptr;
}

void InjectDebug::run() {
  load_dex();
  write_dex();
}

void InjectDebug::load_dex() {
  DexStore root_store("classes");
  root_store.set_dex_magic(load_dex_magic_from_dex(m_dex_files[0].c_str()));
  m_stores.emplace_back(std::move(root_store));

  dex_stats_t input_totals;
  std::vector<dex_stats_t> input_dexes_stats;
  redex::load_classes_from_dexes_and_metadata(m_dex_files, m_stores,
                                              input_totals, input_dexes_stats);
}

void InjectDebug::write_dex() {
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  instruction_lowering::run(m_stores, true);

  for (size_t store_num = 0; store_num < m_stores.size(); ++store_num) {
    DexStore& store = m_stores[store_num];
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      std::string filename =
          redex::get_dex_output_name(m_conf.get_outdir(), store, i);
      DexOutput dout = DexOutput(filename.c_str(), // filename
                                 &store.get_dexen()[i], // classes
                                 nullptr, // locator_index
                                 false, // normal_primary_dex
                                 store_num,
                                 i, // dex_number,
                                 DebugInfoKind::NoCustomSymbolication,
                                 nullptr, // iodi_metadata,
                                 m_conf, // redex options config
                                 pos_mapper.get(), // position_mapper
                                 nullptr, // method_to_id
                                 nullptr, // code_debug_lines
                                 nullptr // post_lowering
      );

      dout.prepare(SortMode::DEFAULT, {SortMode::DEFAULT}, m_conf,
                   m_stores[0].get_dex_magic());
      dout.write();
    }
  }
}
