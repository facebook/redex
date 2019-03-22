/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Reachability.h"

class RemoveUnreachablePass : public Pass {
 public:
  RemoveUnreachablePass() : Pass("RemoveUnreachablePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    m_ignore_sets = reachability::IgnoreSets(jw);
    jw.get("unreachable_removed_symbols", "", m_unreachable_symbols_file_name);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void write_out_removed_symbols(
      const std::string filepath,
      const ConcurrentSet<std::string>& removed_symbols);

 private:
  reachability::IgnoreSets m_ignore_sets;
  std::string m_unreachable_symbols_file_name;
};
