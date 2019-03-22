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
 * Merge Interfaces that have same implementors and interface children.
 * Move interfaces-to-be-merged's functions and fields into merger interfaces
 * if there is no conflict, if there is conflict existed then skip moving for
 * abstract method, rename then move for non-true virtuals. (true virtual
 * conflict will not occur because we checked and removed interfaces that
 * can cause true virtual conflict.)
 * Patch all callsites to merged interfaces (change to merger interfaces.)
 */
class MergeInterfacePass : public Pass {
 public:
  MergeInterfacePass() : Pass("MergeInterfacePass") {}
  void configure_pass(const JsonWrapper& jw) override {
    jw.get("merged_interface_mappings", "", m_merged_interface_mapping_file);
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Metric {
    size_t interfaces_to_merge{0};
    size_t interfaces_created{0};
    size_t interfaces_in_annotation{0};
  } m_metric;

 private:
  std::string m_merged_interface_mapping_file;
};
