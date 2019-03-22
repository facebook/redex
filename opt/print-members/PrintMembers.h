/**
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

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("show_code", false, m_config.show_code);
    jw.get("show_sfields", true, m_config.show_sfields);
    jw.get("show_ifields", true, m_config.show_ifields);

    std::vector<std::string> classes;
    jw.get("only_these_classes", {}, classes);
    for (std::string& name : classes) {
      DexClass* c =
          type_class(DexType::get_type(DexString::get_string(name.c_str())));
      if (c != nullptr) {
        m_config.only_these_classes.insert(c);
      }
    }

    std::vector<std::string> methods;
    jw.get("only_these_methods", {}, methods);
    for (std::string& name : methods) {
      DexMethodRef* m = DexMethod::get_method(name);
      if (m != nullptr && m->is_def()) {
        m_config.only_these_methods.insert(static_cast<DexMethod*>(m));
      }
    }
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
