/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

struct SingleImplConfig {
  std::vector<std::string> white_list;
  std::vector<std::string> package_white_list;
  std::vector<std::string> black_list;
  std::vector<std::string> package_black_list;
  bool intf_anno;
  bool meth_anno;
  bool field_anno;
  bool rename_on_collision;
};

class SingleImplPass : public Pass {
 public:
  SingleImplPass() : Pass("SingleImplPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("white_list", {}, m_pass_config.white_list);
    pc.get("package_white_list", {}, m_pass_config.package_white_list);
    pc.get("black_list", {}, m_pass_config.black_list);
    pc.get("package_black_list", {}, m_pass_config.package_black_list);
    pc.get("type_annotations", true, m_pass_config.intf_anno);
    pc.get("method_annotations", true, m_pass_config.meth_anno);
    pc.get("field_annotations", true, m_pass_config.field_anno);
    pc.get("rename_on_collision", false, m_pass_config.rename_on_collision);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // count of removed interfaces
  size_t removed_count{0};

  // count of invoke-interface changed to invoke-virtual
  static size_t s_invoke_intf_count;

 private:
  SingleImplConfig m_pass_config;
};
