/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace class_merging {

struct ModelSpec;

class AnonymousClassMergingPass : public Pass {
 public:
  AnonymousClassMergingPass() : Pass("AnonymousClassMergingPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ModelSpec m_merging_spec;
  size_t m_min_implementor_size;
};

} // namespace class_merging
