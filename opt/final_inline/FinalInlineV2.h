/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagationWholeProgramState.h"
#include "IRCode.h"
#include "Pass.h"

class FinalInlinePassV2 : public Pass {
 public:
  struct Config {
    std::unordered_set<const DexType*> black_list_types;
    std::unordered_set<std::string> whitelist_method_names;
    bool inline_instance_field;
    Config() : inline_instance_field(false) {}
  };

  FinalInlinePassV2() : Pass("FinalInlinePassV2") {}

  void bind_config() override {
    bind("inline_instance_field", true, m_config.inline_instance_field);
    bind("black_list_types",
         {},
         m_config.black_list_types,
         "List of types that this optimization will omit.");
    bind("whitelist_methods_name_checking_ifields_read",
         {},
         m_config.whitelist_method_names,
         "List of methods names that can be ignored when checking on instance "
         "field read in methods invoked by <init>");
  }

  static size_t run(const Scope&, const Config& config = Config());
  static size_t run_inline_ifields(
      const Scope&,
      const constant_propagation::EligibleIfields& eligible_ifields,
      const Config& config = Config());
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  Config m_config;
};

namespace final_inline {

class class_initialization_cycle : public std::exception {
 public:
  class_initialization_cycle(const DexClass* cls) {
    m_msg = "Found a class initialization cycle involving " + show(cls);
  }

  const char* what() const noexcept override { return m_msg.c_str(); }

 private:
  std::string m_msg;
};

constant_propagation::WholeProgramState analyze_and_simplify_clinits(
    const Scope& scope);

} // namespace final_inline
