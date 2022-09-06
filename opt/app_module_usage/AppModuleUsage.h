/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstddef>
#include <map>
#include <set>
#include <string>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "Pass.h"
#include "ReflectionAnalysis.h"

namespace app_module_usage {
using StoresReferenced =
    std::unordered_map<DexStore*, bool /* used_only_reflectively */>;
using MethodStoresReferenced = ConcurrentMap<DexMethod*, StoresReferenced>;

using Violations = std::map<std::string /* entrypoint */,
                            std::set<std::string /* module name */>>;
} // namespace app_module_usage

/**
 * `AppModuleUsagePass` generates a report of violations of unannotated app
 * module references. The `@UsesAppModule` annotation should be present and
 * contain the name of the module at the entrypoint of an app module, or there
 * is a violation. By default the pass crashes on an occurence of a violation.
 *
 * When configured to continue with `crash_with_violations` set to false a
 * report of all violations is output at
 * "redex-app-module-annotation-violations.csv". Each line of the violation
 * report is the full descriptor of the unannotated entrypoint to a module,
 * followed by the name of the module.
 *
 * By enabling `output_module_use` the pass also generates
 * "redex-app-module-usage.csv" mapping methods to all the app modules used by
 * each method, and "redex-app-module-count.csv" mapping app modules to the
 * number of places it's referenced.
 */
class AppModuleUsagePass : public Pass {
 public:
  AppModuleUsagePass() : Pass("AppModuleUsagePass") {}

  void bind_config() override {
    bind("uses_app_module_annotation_descriptor",
         DexType::get_type("Lcom/facebook/redex/annotations/UsesAppModule;"),
         m_uses_app_module_annotation);
    bind("preexisting_violations_filepath", "",
         m_preexisting_violations_filepath);
    bind("output_module_use", true, m_output_module_use);
    bind("crash_with_violations", false, m_crash_with_violations);
  }

  // Entrypoint for the AppModuleUsagePass pass
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Returns the names of the modules annotated as used by the given entrypoint
  template <typename T>
  static std::unordered_set<std::string_view> get_modules_used(
      T* entrypoint, DexType* annotation_type);

 private:
  void load_preexisting_violations(DexStoresVector&);

  app_module_usage::MethodStoresReferenced analyze_method_xstore_references(
      const Scope& scope);

  ConcurrentMap<DexField*, DexStore*> analyze_field_xstore_references(
      const Scope& scope);

  // Returns number of violations.
  unsigned gather_violations(
      const app_module_usage::MethodStoresReferenced& method_store_refs,
      const ConcurrentMap<DexField*, DexStore*>& field_store_refs,
      app_module_usage::Violations& violations) const;

  // returns true if the given entrypoint name is allowed to use the given store
  bool access_excused_due_to_preexisting(const std::string& entrypoint_name,
                                         DexStore* store_used) const;
  bool access_granted_by_annotation(DexMethod* method, DexStore* target) const;
  bool access_granted_by_annotation(DexField* field, DexStore* target) const;
  bool access_granted_by_annotation(DexClass* cls, DexStore* target) const;

  // Map of violations from entrypoint names to the names of stores used
  // by the entrypoint
  std::unordered_map<std::string, std::unordered_set<DexStore*>>
      m_preexisting_violations;

  // To quickly look up wich DexStore ("module") a DexType is from
  ConcurrentMap<DexType*, DexStore*> m_type_store_map;

  bool m_output_module_use;
  bool m_crash_with_violations;
  DexType* m_uses_app_module_annotation;
  std::string m_preexisting_violations_filepath;
};
