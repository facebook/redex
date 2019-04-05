/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ClassHierarchy.h"
#include "DexClass.h"
#include "InterDexPassPlugin.h"
#include "Model.h"
#include "PassManager.h"

class TypeErasureInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  TypeErasureInterDexPlugin(const std::vector<ModelSpec>& model_spec,
                            PassManager& mgr)
      : m_mgr(mgr) {
    for (auto& spec : model_spec) {
      for (const auto root : spec.roots) {
        m_root_to_model_spec.emplace(root, std::move(spec));
      }
      always_assert_log(spec.enabled, "Only accepting enabled models!\n");
    }
  }

  void configure(const Scope& original_scope, ConfigFiles&) override {
    // NOTE: In case other InterDex plugins (except us)
    //       add new classes to the scope, we aren't
    //       considering them.
    m_scope = original_scope;
    m_type_system = std::make_unique<TypeSystem>(m_scope);
  }

  void add_to_scope(DexClass* cls) { m_scope.push_back(cls); }

  bool should_skip_class(const DexClass* clazz) override;

  bool should_not_relocate_methods_of_class(const DexClass* clazz) override;

  void gather_refs(const interdex::DexInfo& dex_info,
                   const DexClass* cls,
                   std::vector<DexMethodRef*>& mrefs,
                   std::vector<DexFieldRef*>& frefs,
                   std::vector<DexType*>& trefs,
                   std::vector<DexClass*>* erased_classes,
                   bool should_not_relocate_methods_of_class) override;

  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) override;

  DexClasses leftover_classes() override;

  void cleanup(const std::vector<DexClass*>& scope) override;

 private:
  std::unordered_map<const DexType*, ModelSpec> m_root_to_model_spec;
  std::unordered_set<DexType*> m_mergeables_skipped;
  std::unordered_set<DexType*> m_mergeables_selected;
  std::unordered_map<DexType*, TypeSet> m_current_mergeables;
  std::unordered_set<const DexClass*> m_generated_types;
  std::unordered_map<DexType*, std::unordered_set<DexType*>>
      m_cls_to_mergeables;
  std::unordered_map<DexType*, std::unordered_set<const DexClass*>>
      m_mergeable_to_cls;

  Scope m_scope;
  std::unique_ptr<TypeSystem> m_type_system;
  PassManager& m_mgr;

  void filter_extra_mergeables(const DexClasses& classes);
  bool is_mergeable(const DexClass* clazz);
};
