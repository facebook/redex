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

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("rename_annotations", false, m_rename_annotations);
    jw.get("pre_filter_whitelist", {}, m_pre_filter_whitelist);
    jw.get("post_filter_whitelist", {}, m_post_filter_whitelist);
    jw.get("untouchable_hierarchies", {}, m_untouchable_hierarchies);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_rename_annotations;
  std::vector<std::string> m_pre_filter_whitelist;
  std::vector<std::string> m_post_filter_whitelist;
  std::vector<std::string> m_untouchable_hierarchies;
};
