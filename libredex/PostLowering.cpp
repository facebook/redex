/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"

class NoopPostLowering : public PostLowering {
 public:
  void sync() override {}
  void run(const DexStoresVector& stores) override {}
  void finalize(ApkManager& mgr) override {}
};

std::unique_ptr<PostLowering> PostLowering::create() {
  return std::make_unique<NoopPostLowering>();
}
