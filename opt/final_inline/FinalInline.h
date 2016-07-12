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
    pc.get("keep_class_member_annos", {}, m_keep_class_member_annos);
    pc.get("keep_class_members", {}, m_keep_class_members);
    pc.get("remove_class_members", {}, m_remove_class_members);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_keep_class_member_annos;
  std::vector<std::string> m_keep_class_members;
  std::vector<std::string> m_remove_class_members;
};
