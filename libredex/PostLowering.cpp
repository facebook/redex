/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"
#include "DexOutput.h"

class NoopPostLowering : public PostLowering {
 public:
  void run(const DexStoresVector& stores) override {}
  void finalize(AssetManager& mgr) override {}

  void load_dex_indexes(ConfigFiles&,
                        int32_t,
                        DexClasses*,
                        GatheredTypes&,
                        const std::string&,
                        size_t) override {}
};

std::unique_ptr<PostLowering> PostLowering::create() {
  return std::make_unique<NoopPostLowering>();
}
