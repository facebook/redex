/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct SingleImplConfig {
  std::vector<std::string> allowlist;
  std::vector<std::string> package_allowlist;
  std::vector<std::string> blocklist;
  std::vector<std::string> package_blocklist;
  std::vector<std::string> anno_blocklist;
  bool intf_anno;
  bool meth_anno;
  bool field_anno;
  bool rename_on_collision;
  bool filter_proguard_special_interfaces;
};

class SingleImplPass : public Pass {
 public:
  SingleImplPass() : Pass("SingleImplPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  void bind_config() override {
    bind("allowlist", {}, m_pass_config.allowlist);
    bind("package_allowlist", {}, m_pass_config.package_allowlist);
    bind("blocklist", {}, m_pass_config.blocklist);
    bind("package_blocklist", {}, m_pass_config.package_blocklist);
    bind("anno_blocklist", {}, m_pass_config.anno_blocklist);
    bind("type_annotations", true, m_pass_config.intf_anno);
    bind("method_annotations", true, m_pass_config.meth_anno);
    bind("field_annotations", true, m_pass_config.field_anno);
    bind("rename_on_collision", false, m_pass_config.rename_on_collision);
    bind("filter_proguard_special_interfaces",
         false,
         m_pass_config.filter_proguard_special_interfaces);
  }

  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // count of removed interfaces
  size_t removed_count{0};

  // count of invoke-interface changed to invoke-virtual
  static size_t s_invoke_intf_count;

 private:
  SingleImplConfig m_pass_config;
};
