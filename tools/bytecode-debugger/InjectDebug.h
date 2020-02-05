/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "DexLoader.h"

/* Class used to inject debug information into a dex file.
 *
 * Uses Redex libraries to load a dex file into memory and output a new
 * dex file. IODI will be used to add the required debug info items into the
 * dex file.
 */

class InjectDebug {
 public:
  InjectDebug(const std::string& outdir,
              const std::vector<std::string>& dex_files);
  ~InjectDebug();
  void run();

 private:
  const ConfigFiles m_conf;
  const std::vector<std::string> m_dex_files;
  DexStore m_store;

  void load_dex();
  void write_dex();
};
