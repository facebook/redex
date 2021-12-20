/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"
#include "Pass.h"
#include <cstddef>
#include <string>

namespace AppModuleUsage {
struct UseCount {
  unsigned int direct_count{0};
  unsigned int reflective_count{0};
};
} // namespace AppModuleUsage

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
 * By default when the pass does it fail it also generates
 * "redex-app-module-usage.csv" mapping methods to all the app modules used by
 * each method, and "redex-app-module-count.csv" mapping app modules to the
 * number of places it's referenced.
 *
 * Each line of "redex-app-module-usage.csv" is the source module name, followed
 * by the full descriptor of a method, followed by a list of the names of all
 * modules used by the method (each prefixed with "(r)" is used reflectively or
 * "(d&r)" if referenced both directed and reflectively). Each line of
 * "redex-app-module-count.csv" is the name of a module followed by its count of
 * direct references, then its count of reflective references.
 */
class AppModuleUsagePass : public Pass {
 public:
  AppModuleUsagePass() : Pass("AppModuleUsagePass") {}

  void bind_config() override {
    bind("uses_app_module_annotation_descriptor",
         "Lcom/facebook/redex/annotations/UsesAppModule;",
         m_uses_app_module_annotation_descriptor);
    bind("preexisting_violations_filepath", "",
         m_preexisting_violations_filepath);
    bind("output_module_use", false, m_output_module_use);
    bind("crash_with_violations", false, m_crash_with_violations);
  }

  // Entrypoint for the AppModuleUsagePass pass
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Returns the names of the modules annotated as used by the given entrypoint
  template <typename T>
  static std::unordered_set<std::string> get_modules_used(
      T* entrypoint, DexType* annotation_type);

 private:
  void load_preexisting_violations(DexStoresVector&);
  void analyze_direct_app_module_usage(const Scope&, const std::string&);
  void analyze_reflective_app_module_usage(const Scope&, const std::string&);
  // Outputs report of violations, returns the number of violations
  size_t generate_report(const Scope&, const std::string&, PassManager&);

  // returns true if the given entrypoint name is allowed to use the given store
  bool preexisting_access_permitted(const std::string&, DexStore*);

  // Handle a violation of `entrypoint` using `module` unannotated
  template <typename T>
  void violation(T* entrypoint,
                 const std::string& from_module,
                 const std::string& to_module,
                 std::ofstream& ofs,
                 bool print_name);
  // Outputs methods to store mapping to meta file
  void output_usages(const std::string&);
  // Outputs stores to number of uses mapping to meta file
  void output_use_count(const std::string&);
  // Map of count of app modules to the count of times they're used directly
  // and reflectively
  ConcurrentMap<DexStore*, AppModuleUsage::UseCount> m_stores_use_count;

  // Map of all methods to the stores of the modules used by the method
  ConcurrentMap<DexMethod*, std::unordered_set<DexStore*>>
      m_stores_method_uses_map;

  // Map of all methods to the stores of the modules used reflectively by the
  // method
  ConcurrentMap<DexMethod*, std::unordered_set<DexStore*>>
      m_stores_method_uses_reflectively_map;

  // Map of violations from entrypoint names to the names of stores used
  // by the entrypoint
  std::unordered_map<std::string, std::unordered_set<DexStore*>>
      m_preexisting_violations;

  // To quickly look up wich DexStore ("module") a DexType is from
  ConcurrentMap<DexType*, DexStore*> m_type_store_map;

  bool m_output_module_use;
  bool m_crash_with_violations;
  std::string m_uses_app_module_annotation_descriptor;
  std::string m_preexisting_violations_filepath;
};
