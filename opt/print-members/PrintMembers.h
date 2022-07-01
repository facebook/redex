/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "Pass.h"

class PrintMembersPass : public Pass {
 public:
  PrintMembersPass() : Pass("PrintMembersPass") {}

  void bind_config() override {
    bind("show_code", false, m_config.show_code);
    bind("show_sfields", true, m_config.show_sfields);
    bind("show_ifields", true, m_config.show_ifields);
    bind("only_these_classes", {}, m_config.only_these_classes,
         "Only print these classes");
    bind("only_these_methods", {}, m_config.only_these_methods,
         "Only print these methods");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  void handle_method(DexMethod* m, const char* type);
  struct Config {
    bool show_code;
    bool show_sfields;
    bool show_ifields;
    std::unordered_set<DexClass*> only_these_classes;
    std::unordered_set<DexMethod*> only_these_methods;
  } m_config;
};
