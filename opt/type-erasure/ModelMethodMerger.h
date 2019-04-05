/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"
#include "MergerType.h"
#include "Model.h"
#include "TypeTags.h"

struct InterfaceMethod;

namespace dispatch {
struct Spec;
struct DispatchMethod;
} // namespace dispatch

using SwitchIndices = std::set<int>;
using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;
using TypeToMethod = std::unordered_map<const DexType*, const DexMethod*>;
using MergerToField = std::map<const MergerType*, DexField*>;
using MergerToMethods = std::map<const MergerType*, std::vector<DexMethod*>>;
using MethodReplacementPair = std::pair<const std::string, const DexMethod*>;
using TypeToMethodMap =
    std::unordered_map<const DexType*, std::vector<MethodReplacementPair>>;
using MethodToType = std::map<DexMethod*, DexType*, dexmethods_comparator>;

constexpr const char* INSTANCE_OF_STUB_NAME = "$instanceof";

struct MergedMethod {
  std::string name;
  size_t count;
  std::vector<std::string> samples;
};

struct MethodStats {
  std::vector<MergedMethod> merged_methods;
  void add(const MethodOrderedSet& methods);
  void print(const std::string model_name, uint32_t num_mergeables);
};

class ModelMethodMerger {
 public:
  ModelMethodMerger(
      const Scope& scope,
      const std::vector<const MergerType*>& mergers,
      const MergerToField& type_tag_fields,
      const TypeTags* type_tags,
      const std::unordered_map<DexMethod*, std::string>& method_debug_map,
      const ModelSpec& model_spec,
      boost::optional<size_t> max_num_dispatch_target);

  TypeToMethod& merge_methods() {
    merge_ctors();
    dedup_non_ctor_non_virt_methods();
    merge_virt_itf_methods();
    merge_methods_within_shape();
    return get_mergeable_ctor_map();
  }

  const ModelStats& get_stats() const { return m_stats; }
  TypeToMethod& get_mergeable_ctor_map() { return m_mergeable_to_merger_ctor; }
  void print_method_stats(const std::string model_name,
                          uint32_t num_mergeables) {
    m_method_stats.print(model_name, num_mergeables);
  }

  // Helpers
  dispatch::DispatchMethod create_dispatch_method(
      const dispatch::Spec spec, const std::vector<DexMethod*>& targets);

  static DexMethod* create_instantiation_factory(DexType* owner_type,
                                                 std::string name,
                                                 DexProto* proto,
                                                 const DexAccessFlags access,
                                                 DexMethod* ctor);
  static void inline_dispatch_entries(DexMethod* dispatch);

  static std::vector<IRInstruction*> make_string_const(uint16_t dest,
                                                       std::string val);
  static std::vector<IRInstruction*> make_check_cast(DexType* type,
                                                     uint16_t src_dest);

  TypeToMethodMap get_method_dedup_map() { return m_method_dedup_map; }

 private:
  const Scope& m_scope;
  const std::vector<const MergerType*> m_mergers;
  const MergerToField m_type_tag_fields;
  const TypeTags* m_type_tags;
  const std::unordered_map<DexMethod*, std::string>& m_method_debug_map;
  const ModelSpec m_model_spec;
  // This member is only used for testing purpose. If its value is greator than
  // zero, the splitting decision will bypass the instruction count limit.
  boost::optional<size_t> m_max_num_dispatch_target;

  // dmethods
  MergerToMethods m_merger_ctors;
  MergerToMethods m_merger_non_ctors;
  // vmethods
  MergerToMethods m_merger_non_vmethods;
  // merger ctor map
  TypeToMethod m_mergeable_to_merger_ctor;
  // Stats for method dedupping
  MethodStats m_method_stats;
  // Method dedup map
  TypeToMethodMap m_method_dedup_map;

  ModelStats m_stats;

  void merge_ctors();
  void dedup_non_ctor_non_virt_methods();
  void merge_virt_itf_methods();
  void merge_methods_within_shape();
  void fix_visibility();

  void merge_virtual_methods(
      const Scope& scope,
      DexType* super_type,
      DexType* target_type,
      DexField* type_tag_field,
      const std::vector<MergerType::VirtualMethod>& virt_methods,
      std::vector<std::pair<DexClass*, DexMethod*>>& dispatch_methods,
      std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee);

  std::map<SwitchIndices, DexMethod*> get_dedupped_indices_map(
      const std::vector<DexMethod*>& targets);

  DexType* get_merger_type(DexType* mergeable);

  void update_to_static(
      const std::set<DexMethod*, dexmethods_comparator>& methods);

  std::string get_method_signature_string(DexMethod* meth);
};
