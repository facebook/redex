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
#include "DexUtil.h"
#include "Pass.h"

class PrintMembersPass : public Pass {
 public:
  PrintMembersPass() : Pass("PrintMembersPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("show_code", false, m_config.show_code);
    pc.get("show_sfields", true, m_config.show_sfields);
    pc.get("show_ifields", true, m_config.show_ifields);

    std::vector<std::string> classes;
    pc.get("only_these_classes", {}, classes);
    for (std::string& name : classes) {
      DexClass* c =
          type_class(DexType::get_type(DexString::get_string(name.c_str())));
      if (c != nullptr) {
        m_config.only_these_classes.insert(c);
      }
    }

    std::vector<std::string> methods;
    pc.get("only_these_methods", {}, methods);
    for (std::string& name : methods) {
      DexMethodRef* m = DexMethod::get_method(name);
      if (m != nullptr && m->is_def()) {
        m_config.only_these_methods.insert(static_cast<DexMethod*>(m));
      }
    }
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

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
