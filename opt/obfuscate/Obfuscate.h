/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "PassManager.h"

class ObfuscatePass : public Pass {
 public:
  ObfuscatePass() : Pass("ObfuscatePass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};

struct RenameStats {
  size_t fields_total = 0;
  size_t fields_renamed = 0;
  size_t dmethods_total = 0;
  size_t dmethods_renamed = 0;
  size_t vmethods_total = 0;
  size_t vmethods_renamed = 0;
};

void obfuscate(Scope& classes, RenameStats& stats);
