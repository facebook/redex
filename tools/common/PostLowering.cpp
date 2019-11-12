/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"

void PostLowering::setup() {}

void PostLowering::run(const DexClasses& dex) {}

boost::optional<Gatherer> PostLowering::get_secondary_gatherer() {
  return boost::none;
}

void PostLowering::cleanup(PassManager& mgr) {}
