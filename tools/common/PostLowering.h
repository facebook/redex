/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "DexClass.h"
#include "PassManager.h"

class PostLowering {
 public:
  std::vector<DexString*> get_extra_strings(const DexClasses& dex);
  void setup();
  void run(const DexClasses& dex);
  void cleanup(PassManager& mgr);
};
