/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include <string>

class PostLowering {
 public:
  void run(const DexClasses& dex, const std::string& output_dir);
};
