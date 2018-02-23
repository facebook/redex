/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

#include "ClassHierarchy.h"

enum class DontRenameReasonCode {
  Annotated,
  Annotations,
  Specific,
  Packages,
  Hierarchy,
  Resources,
  ClassNameLiterals,
  Canaries,
  NativeBindings,
  SerdeRelationships,
  ClassForTypesWithReflection,
  ProguardCantRename,
};

struct DontRenameReason {
  DontRenameReasonCode code;
  std::string rule;
};

class AliasMap;

class RenameClassesPassV2 : public Pass {
 public:
  RenameClassesPassV2() : Pass("RenameClassesPassV2") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("rename_annotations", false, m_rename_annotations);
    pc.get("force_rename_hierarchies", {}, m_force_rename_hierarchies);
    pc.get("allow_layout_rename_packages", {}, m_allow_layout_rename_packages);
    pc.get("dont_rename_hierarchies", {}, m_dont_rename_hierarchies);
    pc.get("dont_rename_annotated", {}, m_dont_rename_annotated);
    std::vector<std::string> dont_rename_specific;
    pc.get("dont_rename_specific", {}, dont_rename_specific);
    pc.get("dont_rename_packages", {}, m_dont_rename_packages);
    pc.get("dont_rename_types_with_reflection", {},
        m_dont_rename_types_with_reflection);
    m_dont_rename_specific.insert(dont_rename_specific.begin(),
        dont_rename_specific.end());
  }

  virtual void eval_pass(DexStoresVector& stores,
      ConfigFiles& cfg, PassManager& mgr) override;
  virtual void run_pass(DexStoresVector& stores,
      ConfigFiles& cfg, PassManager& mgr) override;

 private:
  std::unordered_map<const DexType*, std::string>
  build_force_rename_hierarchies(PassManager&, Scope&, const ClassHierarchy&);

  std::unordered_set<std::string> build_dont_rename_resources(
    PassManager&, std::unordered_map<const DexType*, std::string>&);
  std::unordered_set<std::string> build_dont_rename_class_name_literals(Scope&);
  std::unordered_set<std::string> build_dont_rename_for_types_with_reflection(
      Scope&, const ProguardMap&);
  std::unordered_set<std::string> build_dont_rename_canaries(Scope&);
  std::unordered_map<const DexType*, std::string> build_dont_rename_hierarchies(
      PassManager&, Scope&, const ClassHierarchy&);
  std::unordered_set<const DexType*> build_dont_rename_native_bindings(
      Scope& scope);
  std::unordered_set<const DexType*> build_dont_rename_serde_relationships(
      Scope& scope);
  std::unordered_set<const DexType*> build_dont_rename_annotated();

  void eval_classes(Scope& scope,
                    const ClassHierarchy& class_hierarchy,
                    ConfigFiles& cfg,
                    bool rename_annotations,
                    PassManager& mgr);
  void eval_classes_post(Scope& scope,
                         const ClassHierarchy& class_hierarchy,
                         PassManager& mgr);
  void rename_classes(Scope& scope,
                      ConfigFiles& cfg,
                      bool rename_annotations,
                      PassManager& mgr);
  void rename_classes_in_layouts(const AliasMap& aliases, PassManager& mgr);

  // Config and rules
  bool m_rename_annotations;
  std::vector<std::string> m_force_rename_hierarchies;
  std::vector<std::string> m_allow_layout_rename_packages;
  std::vector<std::string> m_dont_rename_hierarchies;
  std::vector<std::string> m_dont_rename_annotated;
  std::vector<std::string> m_dont_rename_types_with_reflection;
  std::vector<std::string> m_dont_rename_packages;
  std::unordered_set<std::string> m_dont_rename_specific;

  // Decisions we made in the eval_classes pass
  std::unordered_set<const DexClass*> m_force_rename_classes;
  std::unordered_map<const DexClass*, DontRenameReason> m_dont_rename_reasons;

  std::string m_apk_dir;
};
