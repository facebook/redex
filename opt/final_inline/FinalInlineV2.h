/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include "ConstantPropagationWholeProgramState.h"
#include "DexClass.h"
#include "IRCode.h"
#include "InitClassesWithSideEffects.h"
#include "Pass.h"
#include "PatriciaTreeSetAbstractDomain.h"

class FinalInlinePassV2 : public Pass {
 public:
  struct Config {
    std::unordered_set<const DexType*> blocklist_types;
    std::unordered_set<std::string> allowlist_method_names;
    bool inline_instance_field;
    Config() : inline_instance_field(false) {}
  };

  FinalInlinePassV2() : Pass("FinalInlinePassV2") {}

  void bind_config() override {
    bind("inline_instance_field", true, m_config.inline_instance_field);
    bind("blocklist_types",
         {},
         m_config.blocklist_types,
         "List of types that this optimization will omit.");
    bind("allowlist_methods_name_checking_ifields_read",
         {},
         m_config.allowlist_method_names,
         "List of methods names that can be ignored when checking on instance "
         "field read in methods invoked by <init>");
  }

  struct Stats {
    size_t inlined_count{0};
    size_t init_classes{0};
    size_t possible_cycles{0};
  };
  static Stats run(const Scope&,
                   int min_sdk,
                   const init_classes::InitClassesWithSideEffects&
                       init_classes_with_side_effects,
                   const XStoreRefs*,
                   const Config& config = Config(),
                   std::optional<DexStoresVector*> stores = std::nullopt);
  static Stats run_inline_ifields(
      const Scope&,
      int min_sdk,
      const init_classes::InitClassesWithSideEffects&
          init_classes_with_side_effects,
      const XStoreRefs*,
      const constant_propagation::EligibleIfields& eligible_ifields,
      const Config& config = Config(),
      std::optional<DexStoresVector*> stores = std::nullopt);
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  Config m_config;
};

namespace final_inline {

constant_propagation::WholeProgramState analyze_and_simplify_clinits(
    const Scope& scope,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const std::unordered_set<const DexType*>& blocklist_types,
    const std::unordered_set<std::string>& allowed_opaque_callee_names,
    size_t& clinit_cycles);

class StaticFieldReadAnalysis {
 public:
  using Result = sparta::PatriciaTreeSetAbstractDomain<const DexFieldRef*>;

  StaticFieldReadAnalysis(
      const call_graph::Graph& call_graph,
      const std::unordered_set<std::string>& allowed_opaque_callee_names);

  Result analyze(const DexMethod* method);

 private:
  const call_graph::Graph& m_graph;
  std::unordered_map<const DexMethod*, Result> m_summaries;
  std::unordered_set<const DexMethod*> m_finalized;
  std::unordered_set<const DexMethodRef*> m_allowed_opaque_callees;

  Result analyze(const DexMethod* method,
                 std::unordered_set<const DexMethod*>& pending_methods);
};

call_graph::Graph build_class_init_graph(const Scope& scope);

} // namespace final_inline
