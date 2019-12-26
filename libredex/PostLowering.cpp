/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"

class NoopPostLowering : public PostLowering {
 public:
  void gather_components(std::vector<DexString*>&,
                         std::vector<DexType*>&,
                         std::vector<DexFieldRef*>&,
                         std::vector<DexMethodRef*>&,
                         std::vector<DexCallSite*>&,
                         std::vector<DexMethodHandle*>&,
                         std::vector<DexTypeList*>&,
                         const std::vector<DexClass*>&) const override {}
  void run(const DexClasses& dex) override {}
  void finalize(ApkManager& mgr) override {}
};

std::unique_ptr<PostLowering> PostLowering::create() {
  return std::make_unique<NoopPostLowering>();
}
