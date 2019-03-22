/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "IRCode.h"
#include "Pass.h"

class StripDebugInfoPass : public Pass {
 public:
  StripDebugInfoPass() : Pass("StripDebugInfoPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("cls_whitelist", {}, m_config.cls_patterns);
    jw.get("method_whitelist", {}, m_config.meth_patterns);
    jw.get("use_whitelist", false, m_config.use_whitelist);
    jw.get("drop_all_dbg_info", false, m_config.drop_all_dbg_info);
    jw.get("drop_local_variables", false, m_config.drop_local_variables);
    jw.get("drop_line_numbers", false, m_config.drop_line_nrs);
    jw.get("drop_src_files", false, m_config.drop_src_files);
    jw.get("drop_prologue_end", false, m_config.drop_prologue_end);
    jw.get("drop_epilogue_begin", false, m_config.drop_epilogue_begin);
    jw.get("drop_all_dbg_info_if_empty",
           false,
           m_config.drop_all_dbg_info_if_empty);
    jw.get("drop_synth_aggressive", false, m_config.drop_synth_aggressive);
    jw.get("drop_synth_conservative", false, m_config.drop_synth_conservative);
    jw.get("drop_line_numbers_preceeding_safe",
           false,
           m_config.drop_line_nrs_preceeding_safe);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void set_drop_prologue_end(bool b) { m_config.drop_prologue_end = b; }
  void set_drop_local_variables(bool b) { m_config.drop_local_variables = b; }
  void set_drop_epilogue_begin(bool b) { m_config.drop_epilogue_begin = b; }
  void set_drop_all_debug_info(bool b) { m_config.drop_all_dbg_info = b; }
  void set_drop_line_numbers(bool b) { m_config.drop_line_nrs = b; }

  struct Config {
    std::vector<std::string> cls_patterns;
    std::vector<std::string> meth_patterns;
    bool use_whitelist{false};
    bool drop_all_dbg_info{false};
    bool drop_local_variables{false};
    bool drop_line_nrs{false};
    bool drop_src_files{false};
    bool drop_prologue_end{false};
    bool drop_epilogue_begin{false};
    bool drop_all_dbg_info_if_empty{false};
    bool drop_synth_aggressive{false};
    bool drop_synth_conservative{false};
    bool drop_line_nrs_preceeding_safe{false};
  };

 private:
  Config m_config;
};

namespace strip_debug_info_impl {

struct Stats {
  int num_matches{0};
  int num_pos_dropped{0};
  int num_var_dropped{0};
  int num_prologue_dropped{0};
  int num_epilogue_dropped{0};
  int num_empty_dropped{0};

  Stats& operator+=(const Stats& other);
};

class StripDebugInfo {
 public:
  explicit StripDebugInfo(const StripDebugInfoPass::Config& config)
      : m_config(config) {}

  Stats run(Scope scope);

  Stats run(IRCode&, bool should_drop_synth = false);

 private:
  bool drop_local_variables() const {
    return m_config.drop_local_variables || m_config.drop_all_dbg_info;
  }
  bool drop_prologue() const {
    return m_config.drop_prologue_end || m_config.drop_all_dbg_info;
  }
  bool drop_epilogue() const {
    return m_config.drop_epilogue_begin || m_config.drop_all_dbg_info;
  }
  bool drop_line_numbers() const {
    return m_config.drop_line_nrs || m_config.drop_all_dbg_info;
  }
  bool drop_line_numbers_preceeding_safe() const {
    return m_config.drop_line_nrs_preceeding_safe || m_config.drop_all_dbg_info;
  }
  bool method_passes_filter(DexMethod* meth) const;
  bool should_remove(const MethodItemEntry& mei, Stats& stats);
  bool should_drop_for_synth(const DexMethod*) const;

  const StripDebugInfoPass::Config& m_config;
};

} // namespace strip_debug_info_impl
