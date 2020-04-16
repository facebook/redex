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

InjectDebug::InjectDebug(const std::string& outdir,
                         const std::vector<std::string>& dex_files)
    : m_conf(Json::Value(), outdir),
      m_dex_files(dex_files),
      m_store("classes") {
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
  m_store.set_dex_magic(load_dex_magic_from_dex(m_dex_files[0].c_str()));

  for (const auto& filename : m_dex_files) {
    m_store.add_classes(load_classes_from_dex(filename.c_str()));
  }
}

void InjectDebug::write_dex() {
  const std::string filename =
      m_conf.get_outdir() + "/" + m_store.get_name() + ".dex";
  DexStoresVector stores{m_store};
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  instruction_lowering::run(stores, true);

  for (size_t i = 0; i < m_store.get_dexen().size(); i++) {
    DexOutput dout = DexOutput(filename.c_str(), // filename
                               &m_store.get_dexen()[i], // classes
                               nullptr, // locator_index
                               false, // normal_primary_dex
                               0, // store_number,
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
                 m_store.get_dex_magic());
    dout.write();
  }
}
