/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <json/json.h>
#include <string>
#include <vector>

#include "ConfigFiles.h"
#include "Configurable.h"
#include "DexStore.h"
#include "PassRegistry.h"

class PassManager;

class Pass : public Configurable {
 public:
  Pass(const std::string& name) : m_name(name) {
    PassRegistry::get().register_pass(this);
  }

  virtual ~Pass() {}

  std::string name() const { return m_name; }

  std::string get_config_name() override { return name(); };

  /**
   * All passes' eval_pass are run, and then all passes' run_pass are run. This
   * allows each pass to evaluate its rules in terms of the original input,
   * without other passes changing the identity of classes. You should NOT
   * change anything in the dex stores in eval_pass. There is no protection
   * against doing so, this is merely a convention.
   */

  virtual void eval_pass(DexStoresVector& stores,
                         ConfigFiles& conf,
                         PassManager& mgr){};
  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& conf,
                        PassManager& mgr) = 0;

 private:
  std::string m_name;
};
