/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "ProguardLoader.h"

#include <string>
#include <vector>
#include <folly/dynamic.h>

class PassManager {
 public:
  PassManager(
    const std::vector<Pass*>& passes,
    const std::vector<KeepRule>& rules,
    const folly::dynamic& config = folly::dynamic::object);
  void run_passes(DexClassesVector&);

 private:
  void activate_pass(const char* name, const folly::dynamic& cfg);

  folly::dynamic m_config;
  std::vector<Pass*> m_registered_passes;
  std::vector<Pass*> m_activated_passes;

  //proguard rules
  const std::vector<KeepRule>& m_proguard_rules;
};
