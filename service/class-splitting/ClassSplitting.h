/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"

namespace class_splitting {

void update_coldstart_classes_order(
    ConfigFiles& conf,
    PassManager& mgr,
    const std::unordered_set<DexType*>& coldstart_types,
    const std::vector<std::string>& previously_relocated_types,
    bool log = true);

struct ClassSplittingConfig {
  bool enabled{true};
  bool combine_target_classes_by_api_level{false};
  // Relocated methods per target class when combining by API Level.
  unsigned int relocated_methods_per_target_class{64};
  float method_profiles_appear_percent_threshold{0.01f};
  bool relocate_static_methods{true};
  bool relocate_non_static_direct_methods{true};
  bool relocate_non_true_virtual_methods{true};
  bool relocate_true_virtual_methods{true};
  bool trampolines{true};
  unsigned int trampoline_size_threshold{100};
  std::vector<std::string> blocklist_types;
  // If true, only consider methods that appear in the profiles for relocation.
  bool profile_only{false};
  // If true, also consider source-block info for decision making.
  bool source_blocks{true};
};

struct ClassSplittingStats {
  size_t relocation_classes{0};
  size_t relocated_static_methods{0};
  size_t relocated_non_static_direct_methods{0};
  size_t relocated_non_true_virtual_methods{0};
  size_t relocated_true_virtual_methods{0};
  size_t non_relocated_methods{0};
  size_t popular_methods{0};
  size_t source_block_positive_vals{0};
};

constexpr const char* METRIC_STATICIZED_METHODS =
    "num_class_splitting_staticized_methods";
constexpr const char* METRIC_REWRITTEN_INVOKES =
    "num_class_splitting_rewritten_";
constexpr const char* METRIC_RELOCATION_CLASSES =
    "num_class_splitting_relocation_classes";
constexpr const char* METRIC_RELOCATED_STATIC_METHODS =
    "num_class_splitting_relocated_static_methods";
constexpr const char* METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS =
    "num_class_splitting_relocated_non_static_direct_methods";
constexpr const char* METRIC_RELOCATED_NON_TRUE_VIRTUAL_METHODS =
    "num_class_splitting_relocated_non_true_virtual_methods";
constexpr const char* METRIC_RELOCATED_TRUE_VIRTUAL_METHODS =
    "num_class_splitting_relocated_true_virtual_methods";
constexpr const char* METRIC_NON_RELOCATED_METHODS =
    "num_class_splitting_non_relocated_methods";
constexpr const char* METRIC_POPULAR_METHODS =
    "num_class_splitting_popular_methods";
constexpr const char* METRIC_SOURCE_BLOCKS_POSITIVE_VALS =
    "num_class_splitting_source_block_positive_vals";
constexpr const char* METRIC_RELOCATED_METHODS =
    "num_class_splitting_relocated_methods";
constexpr const char* METRIC_TRAMPOLINES = "num_class_splitting_trampolines";

constexpr const char* RELOCATED_SUFFIX = "$relocated;";

class ClassSplitter final {
 public:
  explicit ClassSplitter(
      const ClassSplittingConfig& config,
      PassManager& mgr,
      const std::unordered_set<DexMethod*>& sufficiently_popular_methods,
      const std::unordered_set<DexMethod*>& insufficiently_popular_methods);
  ClassSplitter() = delete;
  ClassSplitter(const ClassSplitter&) = delete;
  ClassSplitter(ClassSplitter&&) = delete;

  ClassSplitter& operator=(const ClassSplitter&) = delete;
  ClassSplitter& operator=(ClassSplitter&&) = delete;

  void configure(const Scope& scope);
  void prepare(const DexClass* cls,
               std::vector<DexMethodRef*>* mrefs,
               std::vector<DexType*>* trefs,
               bool should_not_relocate_methods_of_class);
  DexClasses additional_classes(const DexClasses& classes);
  void cleanup(const Scope& final_scope);

 private:
  struct RelocatableMethodInfo {
    DexClass* target_cls;
    DexMethod* trampoline_target_method;
    int32_t api_level;
  };

  struct SplitClass {
    std::unordered_map<DexMethod*, RelocatableMethodInfo> relocatable_methods;
  };

  struct TargetClassInfo {
    DexClass* target_cls{nullptr};
    const DexClass* last_source_cls{nullptr};
    size_t size{0}; // number of methods
  };

  bool matches(const std::string& name, const std::string& v);
  bool can_relocate(const DexClass* cls);
  bool has_unresolvable_or_external_field_ref(const DexMethod* m);
  bool can_relocate(bool cls_has_problematic_clinit,
                    const DexMethod* m,
                    bool log,
                    bool* requires_trampoline);
  DexClass* create_target_class(const std::string& target_type_name);
  DexMethod* create_trampoline_method(DexMethod* method,
                                      DexClass* target_cls,
                                      uint32_t api_level);
  bool has_source_block_positive_val(DexMethod* method);
  void materialize_trampoline_code(DexMethod* source, DexMethod* target);

  std::unordered_map<int32_t, TargetClassInfo> m_target_classes_by_api_level;
  size_t m_next_target_class_index{0};
  std::unordered_map<DexType*, DexClass*> m_target_classes_by_source_classes;
  std::unordered_map<const DexClass*, SplitClass> m_split_classes;
  std::vector<std::pair<DexMethod*, DexClass*>> m_methods_to_relocate;
  std::vector<std::pair<DexMethod*, DexMethod*>> m_methods_to_trampoline;
  ClassSplittingStats m_stats;
  std::unordered_set<DexMethod*> m_non_true_virtual_methods;
  ClassSplittingConfig m_config;
  PassManager& m_mgr;
  const std::unordered_set<DexMethod*>& m_sufficiently_popular_methods;
  // Methods that appear in the profiles and whose frequency does not exceed
  // the threashold.
  const std::unordered_set<DexMethod*>& m_insufficiently_popular_methods;
};

} // namespace class_splitting
