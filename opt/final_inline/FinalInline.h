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

class FinalInlinePass : public Pass {
 public:
  FinalInlinePass() : Pass("FinalInlinePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("keep_class_members", {}, m_config.keep_class_members);
    pc.get("remove_class_members", {}, m_config.remove_class_members);
    pc.get(
        "replace_encodable_clinits", false, m_config.replace_encodable_clinits);
    pc.get("propagate_static_finals", false, m_config.propagate_static_finals);
    pc.get("inline_wide_fields", false, m_config.inline_wide_fields);
  }

  static size_t propagate_constants_for_test(Scope& scope,
                                             bool inline_wide_fields);

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Config {
    std::vector<std::string> keep_class_member_annos;
    std::vector<std::string> keep_class_members;
    std::vector<std::string> remove_class_members;
    bool replace_encodable_clinits;
    bool propagate_static_finals;
    bool inline_wide_fields;
  } m_config;
};
