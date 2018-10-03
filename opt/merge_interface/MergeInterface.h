/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "PassManager.h"

/**
 * Merge Interfaces that have same implementors.
 */
class MergeInterfacePass : public Pass {
 public:
  MergeInterfacePass() : Pass("MergeInterfacePass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Metric {
    size_t interfaces_to_merge{0};
    size_t interfaces_merge_left{0};
    size_t interfaces_in_annotation{0};
  } m_metric;
};
