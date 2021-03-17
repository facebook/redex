/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "ConfigFiles.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "IRList.h"
#include "TypeInference.h"

/* Class used to inject debug information into a dex file.
 *
 * Uses Redex libraries to load a dex file into memory, add new debug
 * information, and output a new dex file.
 */

class InjectDebug {
 public:
  InjectDebug(const std::string& outdir,
              const std::vector<std::string>& dex_files);
  ~InjectDebug();
  void run();

 private:
  ConfigFiles m_conf;
  const std::vector<std::string> m_dex_files;
  DexStoresVector m_stores;

  void load_dex();
  void inject_all();
  void write_dex();

  void inject_method(DexMethod* dex_method, int* line_start);
  void inject_register(IRCode* ir_code,
                       const IRList::iterator& ir_it,
                       const type_inference::TypeEnvironment& type_env,
                       reg_t reg);
};
