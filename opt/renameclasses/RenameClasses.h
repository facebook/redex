/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class RenameClassesPass : public Pass {
 public:
  RenameClassesPass() : Pass("RenameClassesPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("rename_annotations", false, m_rename_annotations);
    pc.get("pre_filter_whitelist", {}, m_pre_filter_whitelist);
    pc.get("post_filter_whitelist", {}, m_post_filter_whitelist);
    pc.get("untouchable_hierarchies", {}, m_untouchable_hierarchies);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_rename_annotations;
  std::vector<std::string> m_pre_filter_whitelist;
  std::vector<std::string> m_post_filter_whitelist;
  std::vector<std::string> m_untouchable_hierarchies;
};
