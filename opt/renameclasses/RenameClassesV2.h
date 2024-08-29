/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>

#include "Pass.h"

#include "ClassHierarchy.h"

struct ProguardMap;

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

namespace rewriter {
class TypeStringMap;
} // namespace rewriter

// This data structure models the Android ART's data structure in
// libdexfile/dex/type_lookup_table.cc that is precomputed and used at runtime
// to lookup classes.
class ArtTypeLookupTable {
 public:
  ArtTypeLookupTable(uint32_t size,
                     const std::vector<uint32_t>& initial_hashes);

  bool has_bucket(uint32_t hash) const;

  void insert(uint32_t hash);

 private:
  uint32_t GetPos(uint32_t hash) const { return hash & m_mask; }

  uint32_t m_mask;
  boost::dynamic_bitset<> m_buckets;
};

class RenameClassesPassV2 : public Pass {
 public:
  RenameClassesPassV2() : Pass("RenameClassesPassV2") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
        {RenameClass, EstablishesAndRequiresFinally},
    };
  }

  void bind_config() override {
    bind("rename_annotations", false, m_rename_annotations);
    bind("force_rename_hierarchies", {}, m_force_rename_hierarchies);
    bind("allow_layout_rename_packages", {}, m_allow_layout_rename_packages);
    bind("dont_rename_hierarchies", {}, m_dont_rename_hierarchies);
    bind("dont_rename_annotated", {}, m_dont_rename_annotated);
    bind("dont_rename_specific", {}, m_dont_rename_specific);
    bind("dont_rename_packages", {}, m_dont_rename_packages);
    bind("dont_rename_types_with_reflection", {},
         m_dont_rename_types_with_reflection);
    bind("package_prefix", "", m_package_prefix);
    bind("avoid_type_lookup_table_collisions", false,
         m_avoid_type_lookup_table_collisions);
    trait(Traits::Pass::unique, true);
  }

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

  std::unordered_set<DexClass*> get_renamable_classes(Scope& scope,
                                                      ConfigFiles& conf,
                                                      PassManager& mgr);

  void eval_classes_post(Scope& scope,
                         const ClassHierarchy& class_hierarchy,
                         PassManager& mgr);

 private:
  std::unordered_set<const DexType*> build_force_rename_hierarchies(
      PassManager&, Scope&, const ClassHierarchy&);

  std::unordered_set<std::string> build_dont_rename_class_name_literals(Scope&);
  std::unordered_set<const DexString*>
  build_dont_rename_for_types_with_reflection(Scope&, const ProguardMap&);
  std::unordered_set<const DexString*> build_dont_rename_canaries(Scope&);
  std::unordered_map<const DexType*, const DexString*>
  build_dont_rename_hierarchies(PassManager&, Scope&, const ClassHierarchy&);
  std::unordered_set<const DexType*> build_dont_rename_native_bindings(
      Scope& scope);
  std::unordered_set<const DexType*> build_dont_rename_serde_relationships(
      Scope& scope);
  std::unordered_set<const DexType*> build_dont_rename_annotated();

  void eval_classes(Scope& scope,
                    const ClassHierarchy& class_hierarchy,
                    ConfigFiles& conf,
                    bool rename_annotations,
                    PassManager& mgr);

  std::unordered_set<DexClass*> get_renamable_classes(Scope& scope);

  std::unordered_set<DexClass*> get_unrenamable_classes(
      Scope& scope,
      const std::unordered_set<DexClass*>& renamable_classes,
      PassManager& mgr);

  rewriter::TypeStringMap get_name_mapping(
      const DexStoresVector& stores,
      size_t digits,
      const std::unordered_set<DexClass*>& unrenamable_classes);
  void evolve_name_mapping(
      size_t digits,
      const DexClasses& dex,
      const std::unordered_set<DexClass*>& unrenamable_classes,
      rewriter::TypeStringMap* name_mapping,
      uint32_t* nextGlobalClassIndex);

  rewriter::TypeStringMap get_name_mapping_avoiding_collisions(
      const DexStoresVector& stores,
      const std::unordered_set<DexClass*>& unrenamable_classes,
      size_t* digits,
      size_t* avoided_collisions,
      size_t* skipped_indices_count);
  bool evolve_name_mapping_avoiding_collisions(
      size_t digits,
      const DexClasses& dex,
      const std::unordered_set<DexClass*>& unrenamable_classes,
      uint32_t index_end,
      rewriter::TypeStringMap* name_mapping,
      uint32_t* next_index,
      std::set<uint32_t>* skipped_indices,
      size_t* avoided_collisions);

  void rename_classes(Scope& scope,
                      const rewriter::TypeStringMap& name_mapping,
                      PassManager& mgr);
  void rename_classes_in_layouts(const rewriter::TypeStringMap& name_mapping,
                                 PassManager& mgr);

  std::string prepend_package_prefix(const char* descriptor);

  int m_base_strings_size = 0;
  int m_ren_strings_size = 0;

  // Config and rules
  bool m_rename_annotations;
  std::vector<std::string> m_force_rename_hierarchies;
  std::vector<std::string> m_allow_layout_rename_packages;
  std::vector<std::string> m_dont_rename_hierarchies;
  std::vector<std::string> m_dont_rename_annotated;
  std::vector<std::string> m_dont_rename_types_with_reflection;
  std::vector<std::string> m_dont_rename_packages;
  std::unordered_set<std::string> m_dont_rename_specific;
  std::string m_package_prefix;

  // Decisions we made in the eval_classes pass
  std::unordered_map<const DexClass*, DontRenameReason> m_dont_rename_reasons;

  // State for ensuring xml files are rewritten properly.
  std::unordered_set<const DexString*> m_renamable_layout_classes;

  std::string m_apk_dir;

  bool m_avoid_type_lookup_table_collisions = false;
};
