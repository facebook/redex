/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"

void PostLowering::setup() {}

void PostLowering::run(const DexClasses& dex) {}

std::vector<DexString*> PostLowering::get_extra_strings(const DexClasses& dex) {
  std::vector<DexString*> empty;
  return empty;
}

void PostLowering::cleanup(PassManager& mgr) {}
