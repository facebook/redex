/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"
#include "IRCode.h"

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
    pc.get("drop_prologue_end", false, m_drop_prologue_end);
    pc.get("drop_epilogue_begin", false, m_drop_epilogue_begin);
    pc.get("drop_all_dbg_info_if_empty", false, m_drop_all_dbg_info_if_empty);
    pc.get("drop_synth_aggressive", false, m_drop_synth_aggressive);
    pc.get("drop_synth_conservative", false, m_drop_synth_conservative);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void set_drop_prologue_end(bool b) { m_drop_prologue_end = b; }
  void set_drop_local_variables(bool b) { m_drop_local_variables = b; }
  void set_drop_epilogue_begin(bool b) { m_drop_epilogue_begin = b; }
  void set_drop_all_debug_info(bool b) { m_drop_all_dbg_info = b; }
  void set_drop_line_numbers(bool b) { m_drop_line_nrs = b; }

 private:
  bool drop_local_variables() const {
    return m_drop_local_variables || m_drop_all_dbg_info;
  }
  bool drop_prologue() const {
    return m_drop_prologue_end || m_drop_all_dbg_info;
  }
  bool drop_epilogue() const {
    return m_drop_epilogue_begin || m_drop_all_dbg_info;
  }
  bool drop_line_numbers() const {
    return m_drop_line_nrs || m_drop_all_dbg_info;
  }
  bool method_passes_filter(DexMethod* meth) const;
  bool should_remove(const MethodItemEntry& mei);
  bool should_drop_for_synth(const DexMethod*) const;

  std::vector<std::string> m_cls_patterns;
  std::vector<std::string> m_meth_patterns;
  bool m_use_whitelist = false;
  bool m_drop_all_dbg_info = false;
  bool m_drop_local_variables = false;
  bool m_drop_line_nrs = false;
  bool m_drop_src_files = false;
  bool m_drop_prologue_end = false;
  bool m_drop_epilogue_begin = false;
  bool m_drop_all_dbg_info_if_empty = false;
  bool m_drop_synth_aggressive = false;
  bool m_drop_synth_conservative = false;
  int m_num_matches = 0;
  int m_num_pos_dropped = 0;
  int m_num_var_dropped = 0;
  int m_num_prologue_dropped = 0;
  int m_num_epilogue_dropped = 0;
  int m_num_empty_dropped = 0;
};
