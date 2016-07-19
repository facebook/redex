// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "Pass.h"

class StripDebugInfoPass : public Pass {
 public:
  StripDebugInfoPass() : Pass("StripDebugInfoPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("cls_whitelist", {}, m_cls_patterns);
    pc.get("method_whitelist", {}, m_meth_patterns);
    pc.get("use_whitelist", false, m_use_whitelist);
    pc.get("drop_all_dbg_info", false, m_drop_all_dbg_info);
    pc.get("drop_local_variables", false, m_drop_local_variables);
    pc.get("drop_line_numbers", false, m_drop_line_nrs);
    pc.get("drop_src_files", false, m_drop_src_files);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_cls_patterns;
  std::vector<std::string> m_meth_patterns;
  bool m_use_whitelist;
  bool m_drop_all_dbg_info;
  bool m_drop_local_variables;
  bool m_drop_line_nrs;
  bool m_drop_src_files;
};
