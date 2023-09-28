/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
                  const IRList::iterator& sget,
                  const IRList::iterator& sput,
                  DexField* field)
      : clinit(clinit), sget(sget), sput(sput), field(field) {}
};

class FinalInlinePass : public Pass {
 public:
  FinalInlinePass() : Pass("FinalInlinePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, RequiresAndPreserves},
    };
  }

  void bind_config() override {
    bind("blocklist_annos",
         {},
         m_config.blocklist_annos,
         "List of annotations, which when applied, will cause this "
         "optimization to omit the annotated element.");
    bind("blocklist_types",
         {},
         m_config.blocklist_types,
         "List of types that this optimization will omit.");
    bind("keep_class_member_annos",
         {},
         m_config.keep_class_member_annos,
         "List of annotations, which when applied, will cause this "
         "optimization to keep the annotated element.");
    bind("keep_class_members", {}, m_config.keep_class_members);
    bind("remove_class_members", {}, m_config.remove_class_members);
    bind(
        "replace_encodable_clinits", false, m_config.replace_encodable_clinits);
    bind("propagate_static_finals", false, m_config.propagate_static_finals);
  }

  static size_t propagate_constants_for_test(Scope& scope,
                                             bool inline_string_fields,
                                             bool inline_wide_fields);

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Config {
    std::vector<DexType*> blocklist_annos;
    std::vector<DexType*> blocklist_types;
    std::vector<DexType*> keep_class_member_annos;
    std::vector<std::string> keep_class_members;
    std::vector<std::string> remove_class_members;
    bool replace_encodable_clinits;
    bool propagate_static_finals;
  } m_config;

  static void inline_fields(const Scope& scope);
  static void inline_fields(const Scope& scope, Config& config);
  static std::unordered_map<DexField*, std::vector<FieldDependency>>
  find_dependencies(const Scope& scope,
                    DexMethod* method,
                    FinalInlinePass::Config& config);
};
