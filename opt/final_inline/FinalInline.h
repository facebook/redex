/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRCode.h"
#include "Pass.h"

struct FieldDependency {
  DexMethod* clinit;
  IRList::iterator sget;
  IRList::iterator sput;
  DexField* field;

  FieldDependency(DexMethod* clinit,
                  IRList::iterator sget,
                  IRList::iterator sput,
                  DexField* field)
      : clinit(clinit), sget(sget), sput(sput), field(field) {}
};

class FinalInlinePass : public Pass {
 public:
  FinalInlinePass() : Pass("FinalInlinePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    std::vector<std::string> temp_config_list;
    jw.get("black_list_annos", {}, temp_config_list);
    for (const auto& type_s : temp_config_list) {
      DexType* type = DexType::get_type(type_s.c_str());
      if (type != nullptr) {
        m_config.black_list_annos.emplace_back(type);
      }
    }

    temp_config_list.clear();
    jw.get("black_list_types", {}, temp_config_list);
    for (const auto& type_s : temp_config_list) {
      DexType* type = DexType::get_type(type_s.c_str());
      if (type != nullptr) {
        m_config.black_list_types.emplace_back(type);
      }
    }

    temp_config_list.clear();
    jw.get("keep_class_member_annos", {}, temp_config_list);
    for (const auto& type_s : temp_config_list) {
      DexType* type = DexType::get_type(type_s.c_str());
      if (type != nullptr) {
        m_config.keep_class_member_annos.emplace_back(type);
      }
    }

    jw.get("keep_class_members", {}, m_config.keep_class_members);
    jw.get("remove_class_members", {}, m_config.remove_class_members);
    jw.get(
        "replace_encodable_clinits", false, m_config.replace_encodable_clinits);
    jw.get("propagate_static_finals", false, m_config.propagate_static_finals);
  }

  static size_t propagate_constants_for_test(Scope& scope,
                                             bool inline_string_fields,
                                             bool inline_wide_fields);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Config {
    std::vector<DexType*> black_list_annos;
    std::vector<DexType*> black_list_types;
    std::vector<DexType*> keep_class_member_annos;
    std::vector<std::string> keep_class_members;
    std::vector<std::string> remove_class_members;
    bool replace_encodable_clinits;
    bool propagate_static_finals;
  } m_config;

  static void inline_fields(const Scope& scope);
  static void inline_fields(const Scope& scope, Config& config);
  static const std::unordered_map<DexField*, std::vector<FieldDependency>>
  find_dependencies(const Scope& scope,
                    DexMethod* method,
                    FinalInlinePass::Config& config);
};
