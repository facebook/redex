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
    pc.get("keep_class_members", {}, m_keep_class_members);
    pc.get("remove_class_members", {}, m_remove_class_members);
    pc.get("replace_encodable_clinits", false, m_replace_encodable_clinits);
    pc.get("propagate_static_finals", false, m_propagate_static_finals);
  }

  static size_t propagate_constants(Scope& scopes);

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_keep_class_member_annos;
  std::vector<std::string> m_keep_class_members;
  std::vector<std::string> m_remove_class_members;
  bool m_replace_encodable_clinits;
  bool m_propagate_static_finals;
};
