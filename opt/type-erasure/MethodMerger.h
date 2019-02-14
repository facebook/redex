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

class MethodMerger {
 public:
  MethodMerger(
      const Scope& scope,
      const std::vector<const MergerType*>& mergers,
      const MergerToField& type_tag_fields,
      const TypeTags* type_tags,
      const std::unordered_map<DexMethod*, std::string>& method_debug_map,
      bool use_external_type_tags,
      bool generate_type_tags,
      bool devirtualize_enabled,
      bool process_method_meta,
      boost::optional<size_t> max_num_dispatch_target,
      bool keep_debug_info);

  TypeToMethod& merge_methods() {
    merge_ctors();
    merge_non_ctor_non_virt_methods();
    merge_virt_itf_methods();
    return get_mergeable_ctor_map();
  }

  uint32_t get_num_ctor_dedupped() { return m_num_ctor_dedupped; }
  uint32_t get_num_static_non_virt_dedupped() {
    return m_num_static_non_virt_dedupped;
  }
  uint32_t get_num_vmethods_dedupped() { return m_num_vmethods_dedupped; }
  uint32_t get_num_const_lifted_methods() { return m_num_const_lifted_methods; }
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

  static IRInstruction* make_load_const(uint16_t dest, size_t val);
  static std::vector<IRInstruction*> make_string_const(uint16_t dest,
                                                       std::string val);
  static std::vector<IRInstruction*> make_check_cast(DexType* type,
                                                     uint16_t src_dest);
  static void patch_callsite(DexMethod* caller,
                             IRInstruction* call_insn,
                             DexMethod* callee,
                             uint32_t type_tag,
                             bool with_type_tag = false);

  TypeToMethodMap get_method_dedup_map() { return m_method_dedup_map; }

 private:
  const Scope& m_scope;
  const std::vector<const MergerType*> m_mergers;
  const MergerToField m_type_tag_fields;
  const TypeTags* m_type_tags;
  const std::unordered_map<DexMethod*, std::string>& m_method_debug_map;
  bool m_use_external_type_tags;
  bool m_generate_type_tags;
  bool m_devirtualize_enabled;
  bool m_process_method_meta;
  // This member is only used for testing purpose. If its value is greator than
  // zero, the splitting decision will bypass the instruction count limit.
  boost::optional<size_t> m_max_num_dispatch_target;
  bool m_keep_debug_info;

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

  uint32_t m_num_ctor_dedupped = 0;
  uint32_t m_num_static_non_virt_dedupped = 0;
  uint32_t m_num_vmethods_dedupped = 0;
  uint32_t m_num_const_lifted_methods = 0;

  void merge_ctors();
  void merge_non_ctor_non_virt_methods();
  void merge_virt_itf_methods();
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

  bool no_type_tags();

  std::string get_method_signature_string(DexMethod* meth);
};
