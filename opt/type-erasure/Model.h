/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "ApproximateShapeMerging.h"
#include "DexClass.h"
#include "MergerType.h"
#include "TypeSystem.h"

using TypeToTypeSet = std::unordered_map<const DexType*, TypeSet>;

enum InterDexGroupingType {
  DISABLED = 0, // No interdex grouping.
  NON_HOT_SET = 1, // Exclude hot set.
  FULL = 2, // Apply interdex grouping on the entire input.
};

/**
 * A class hierarchy specification to model for erasure.
 * This is normally specified via config entries:
 * // array of models
 * "models" : [
 *   {
 *     // this field is really not needed as we could remove the whole entry
 *     // but it's here for simplicity
 *     "enabled" : true,
 *     // this only makes sense when enabled is 'false' and it's intended
 *     // to perform the analysis without the optmization.
 *     // Look at the print comment in the .cpp file to see how to read the
 *     // analysis results
 *     "analysis" : true,
 *     // model name for printing/tracing/debugging purposes
 *     "name" : "Generated Code",
 *     // prefix to every generated class name for this model.
 *     // It's also used for metrics.
 *     // Makes it easy to see what is what
 *     "class_name_prefix" : "GenCode",
 *     // the generated model needs a type tag
 *     "needs_type_tag" : true;
 *     // the model has a type tag predefined and usable
 *     "has_type_tag" : true;
 *     "needs_type_tag" : true;
 *     // build MergerType only for groups that have more than min_count
 *     // classes, ignore others (default to 1)
 *     min_group_count: 100,
 *     // root to the model, the base type to identify all classes
 *     // that are candidate for erasure
 *     "root" : "Lcom/facebook/gencode/BaseType;",
 *     // exclude classes, can be classes or interfaces
 *     "exclude" : [
 *       "Lcom/facebook/gencode/ExcludedBase;"
 *     ],
 *     // a specification for the generated set that is treated specially
 *     // for reference analysis
 *     "generated" : {
 *       // Treat types under the same namespace specially.
 *       // Skip type exclusion check under the same namespace.
 *       // Assuming cross referencing under the same namespace are safe.
 *       "namespace" : true,
 *       // other roots from which identify types that have
 *       // to be treated specially
 *       "other_roots" : [
 *         "Lcom/facebook/gencode/OtherBase;"
 *       ]
 *     }
 *   },
 * ]
 */
struct ModelSpec {
  // whether the spec is to be used
  bool enabled{true};
  // name of the spec for debug/printing
  std::string name;
  // set of roots from which to find all model types
  TypeSet roots;
  // types to exclude from the model
  std::unordered_set<DexType*> exclude_types;
  // prefix for class generation
  std::string class_name_prefix;
  // needs a type tag
  bool needs_type_tag{true};
  // has a predefined and usable type tag
  bool has_type_tag{false};
  // pass an additional type tag param to ctor
  bool pass_additional_type_tag_to_ctor{true};
  // minimum nuber of mergeables to make it into a MergerType
  // (no optimization otherwise)
  size_t min_count{1};
  // set of generated types
  std::unordered_set<DexType*> gen_types;
  // set of annotations marking generated code
  std::unordered_set<DexType*> gen_annos;
  // whether to perform type erasure per dex. If set to true, this would be
  // handled at InterDex level, thorough TypeErasureInterDexPlugin.
  bool dex_sharding{false};
  // Group splitting. This is looser than the per dex split and takes into
  // account the interdex order (if any provided).
  InterDexGroupingType merge_per_interdex_set{InterDexGroupingType::DISABLED};
  // whether to perform type erasure on the primary dex.
  bool include_primary_dex{false};
  // Devirtualize/staticize non-virtual methods
  bool devirtualize_non_virtuals{false};
  // Merge static methods within shape.
  bool merge_static_methods_within_shape{false};
  // Merge direct methods within shape.
  bool merge_direct_methods_within_shape{false};
  // Merge nonvirt methods within shape.
  bool merge_nonvirt_methods_within_shape{false};
  // Process @MethodMeta annotations
  bool process_method_meta{false};
  // Max mergeable count per merger type
  boost::optional<size_t> max_count{boost::none};
  // Approximate shaping
  Json::Value approximate_shape_merging;
  // Allows merging classes with non-primitive static fields. Enabling this will
  // change initialization order.
  bool merge_types_with_static_fields{false};
  // Preserve debug info like line numbers.
  bool keep_debug_info{false};
  // Exclude types with references to Android SDK types. The referenced type may
  // or may not exist depending on the Android version the app runs on. That
  // could cause us to merge a class that fails to verify on certain versions of
  // Android. In this situation the entire merger type will fail to verify.
  Json::Value exclude_reference_to_android_sdk;
};

/**
 * A Model is a revised hierarchy for the class set under analysis.
 * The purpose is to define a small number of types that can be used to
 * merge a set of other types. The mergeables types will be erased.
 * The model takes into account interfaces and shapes of the types
 * to merge in order to define proper aggregation.
 * The Model retains all the class hierarchy and mergeable type information
 * that can be use to generated proper code.
 * Manipulation of the Model is done via calls to the Model public API.
 */
class Model {
 public:
  struct Metrics {
    size_t all_types{0};
    size_t non_mergeables{0};
    size_t excluded{0};
    size_t dropped{0};
  } m_metric;

  // Stats for approximate shape merging
  ApproximateStats m_approx_stats;

 public:
  /**
   * Build a Model given a scope and a specification.
   */
  static Model build_model(const Scope& scope,
                           const DexStoresVector& stores,
                           const ModelSpec& spec,
                           ConfigFiles& cfg);
  static Model build_model(const Scope& scope,
                           const ModelSpec& spec,
                           const TypeSet& types);

  static void update_model(Model& model);

  const std::string get_name() const { return m_spec.name; }
  const std::vector<const DexType*> get_roots() const {
    std::vector<const DexType*> res;
    for (const auto root_merger : m_roots) {
      res.push_back(root_merger->type);
    }
    return res;
  }

  template <class HierarchyWalkerFn = void(const MergerType&)>
  void walk_hierarchy(HierarchyWalkerFn walker) {
    for (const auto root_merger : m_roots) {
      if (!root_merger->dummy) {
        walker(*root_merger);
      }
      walk_hierarchy_helper(walker, root_merger->type);
    }
  }

  const DexType* get_parent(const DexType* child) const {
    auto it = m_parents.find(child);
    if (it == m_parents.end()) {
      return nullptr;
    }
    return it->second;
  }

  const TypeSet& get_interfaces(const DexType* type) const {
    const auto& intfs = m_class_to_intfs.find(type);
    return intfs != m_class_to_intfs.end() ? intfs->second : empty_set;
  }

  const std::string& get_class_name_prefix() const {
    return m_spec.class_name_prefix;
  }

  bool is_dex_sharding_enabled() const { return m_spec.dex_sharding; }

  bool is_merge_per_interdex_set_enabled() const {
    return m_spec.merge_per_interdex_set != InterDexGroupingType::DISABLED;
  }

  const ModelSpec get_model_spec() const { return m_spec; }

  bool needs_type_tag() const { return m_spec.needs_type_tag; }
  bool has_type_tag() const { return m_spec.has_type_tag; }
  bool devirtualize_non_virtuals() const {
    return m_spec.devirtualize_non_virtuals;
  }
  bool process_method_meta() const { return m_spec.process_method_meta; }
  bool keep_debug_info() const { return m_spec.keep_debug_info; }

  void update_redex_stats(PassManager& mgr) const;

  // output directory
  static std::string s_outdir;

  static void build_interdex_groups(ConfigFiles* cfg);

  /**
   * Print everything about the model.
   * The printing has a format to allow grep to isolate specific parts.
   * The format is the following:
   * + TypeName type_info
   * - ErasedTypeName type_info
   * -* MergedType fields
   * -# MergedType methods
   * type_info gives info on children, interfaces and method count.
   * '+' can be used to look at hierarchies of types
   * (i.e. grep -e "^+* L.*;")
   * + Base children(k), interfaces(n), Intf1, Intf2
   * ++ Derived1
   * +++ Derived11
   * ++ Derived2
   * +++ Derived21
   * adding '-' would give the hierarchy and the merged/erasable types
   * (i.e. grep -e "^+* L.*;\|^-* L.*;")
   * + Base
   * ++ Derived1
   * +++ Derived11
   * ++ Shape
   * -- Erasable1
   * -- Erasable2
   * -- Erasable3
   * you can view the hierarchy with the merged types and the fields
   * and methods in the merger
   * (i.e. grep -e "^+* L.*;\|^-.* L.*;")
   * + Base
   * ++ Derived1
   * +++ Derived11
   * ++ Shape
   * -- Erasable1
   * --* field
   * --# method
   */
  std::string print() const;

 private:
  static const TypeSet empty_set;

  // the spec for this model
  ModelSpec m_spec;

  // the roots (base types) for the model
  std::vector<MergerType*> m_roots;
  // all types in this model
  TypeSet m_types;
  // the new generated class hierarchy during analysis.
  // Types are not changed during analysis and m_hierarchy represents
  // the class hierarchy as known to the analysis and what the final
  // hierarchy will be
  ClassHierarchy m_hierarchy;
  // child to parent relationship of types in the model.
  // Because nothing is changed during analysis DexClass::get_super_class()
  // may not have the correct relationship
  std::unordered_map<const DexType*, const DexType*> m_parents;
  // class to interfaces map as known to the analysis
  TypeToTypeSet m_class_to_intfs;
  // interface to class relationship as known to the analysis
  TypeToTypeSet m_intf_to_classes;
  // type to merger map
  std::unordered_map<const DexType*, MergerType> m_mergers;
  // Types excluded by the ModelSpec.exclude_types
  TypeSet m_excluded;
  // The set of non mergeables types. Those are types that are not
  // erasable for whatever reason
  TypeSet m_non_mergeables;

  const TypeSystem& m_type_system;

  static size_t s_shape_count;

  // Used to differentiate between mergers generated per dex.
  // This won't actually be the dex #, but will represent the
  // # of dexes we generated mergers for.
  static size_t s_dex_count;

  // Number of merger types created with the same shape per model.
  std::map<MergerType::Shape, size_t, MergerType::ShapeComp> m_shape_to_count;

  const Scope& m_scope;

  static std::unordered_map<DexType*, size_t> s_cls_to_interdex_group;
  static size_t s_num_interdex_groups;

 private:
  /**
   * Build a Model given a set of roots and a set of types deriving from the
   * roots.
   */
  Model(const Scope& scope,
        const DexStoresVector& stores,
        const ModelSpec& spec,
        const TypeSystem& type_system,
        ConfigFiles& cfg);
  Model(const Scope& scope,
        const ModelSpec& spec,
        const TypeSystem& type_system,
        const TypeSet& types);
  void init(const Scope& scope,
            const ModelSpec& spec,
            const TypeSystem& type_system,
            ConfigFiles* cfg = nullptr);

  void build_hierarchy(const TypeSet& roots);
  void build_interface_map(const DexType* type, TypeSet implemented);
  MergerType* build_mergers(const DexType* root);
  void exclude_types(const std::unordered_set<DexType*>& exclude_types);
  void find_non_mergeables(const Scope& scope, const TypeSet& generated);
  void find_non_root_store_mergeables(const DexStoresVector& stores,
                                      bool include_primary_dex);

  // MergerType creator helpers
  MergerType& create_merger(const DexType* type);
  MergerType& create_dummy_merger(const DexType* type);
  void create_dummy_mergers_if_children(const DexType* type);
  MergerType& create_merger_shape(const DexType* shape_type,
                                  const MergerType::Shape& shape,
                                  const DexType* parent,
                                  const TypeSet& intfs,
                                  const TypeSet& classes);
  MergerType& create_merger_helper(
      const DexType* merger_type,
      const MergerType::Shape& shape,
      const TypeSet& group_key,
      const TypeSet& group_values,
      const boost::optional<size_t>& dex_num,
      const boost::optional<size_t>& interdex_subgroup_idx,
      const boost::optional<size_t>& subgroup_idx);
  void create_mergers_helper(
      const DexType* merger_type,
      const MergerType::Shape& shape,
      const TypeSet& group_key,
      const TypeSet& group_values,
      const boost::optional<size_t>& dex_num,
      const boost::optional<size_t>& interdex_subgroup_idx,
      const boost::optional<size_t>& max_mergeables_count);

  // make shapes out of the model classes
  void shape_model();
  void shape_merger(const MergerType& root, MergerType::ShapeCollector& shapes);
  void approximate_shapes(MergerType::ShapeCollector& shapes);
  void break_by_interface(const MergerType& merger,
                          const MergerType::Shape& shape,
                          MergerType::ShapeHierarchy& hier);
  void flatten_shapes(const MergerType& merger,
                      MergerType::ShapeCollector& shapes);
  std::vector<TypeSet> group_per_interdex_set(const TypeSet& types);
  void map_fields(MergerType& shape, const TypeSet& classes);

  // collect and distribute methods across MergerTypes
  void collect_methods();
  void add_virtual_scope(MergerType& merger, const VirtualScope& virt_scope);
  void add_interface_scope(MergerType& merger, const VirtualScope& intf_scope);
  void distribute_virtual_methods(const DexType* type,
                                  std::vector<const VirtualScope*> virt_meths);

  // Model internal type system helpers
  void set_parent_child(const DexType* parent, const DexType* child) {
    m_hierarchy[parent].insert(child);
    m_parents[child] = parent;
  }

  void remove_child(const DexType* child) {
    const auto& prev_parent_hier = m_hierarchy.find(m_parents[child]);
    always_assert(prev_parent_hier != m_hierarchy.end());
    auto erased = prev_parent_hier->second.erase(child);
    always_assert(erased > 0);
    if (prev_parent_hier->second.empty()) {
      m_hierarchy.erase(prev_parent_hier);
    }
  }

  void move_child_to_mergeables(MergerType& merger, const DexType* child) {
    TRACE(TERA, 3, "Adding child %s to merger %s\n", SHOW(child),
          print(&merger).c_str());
    remove_child(child);
    merger.mergeables.insert(child);
  }

  // printers
  std::string print(const MergerType* merger) const;
  std::string print(const DexType* type) const;
  std::string print(const DexType* type, int nest) const;

  // walker helper
  template <class HierarchyWalkerFn = void(const MergerType&)>
  void walk_hierarchy_helper(HierarchyWalkerFn walker, const DexType* type) {
    const auto& children = m_hierarchy.find(type);
    if (children == m_hierarchy.end()) return;
    for (const auto& child : children->second) {
      const auto& merger = m_mergers.find(child);
      if (merger != m_mergers.end() && !merger->second.dummy) {
        walker(merger->second);
      }
      walk_hierarchy_helper(walker, child);
    }
  }
};

struct ModelStats {
  uint32_t m_num_classes_merged = 0;
  uint32_t m_num_generated_classes = 0;
  uint32_t m_num_ctor_dedupped = 0;
  uint32_t m_num_static_non_virt_dedupped = 0;
  uint32_t m_num_vmethods_dedupped = 0;
  uint32_t m_num_const_lifted_methods = 0;
  // Stats of methods merging within each class. They are number of merged
  // methods minus number of dispatch methods.
  uint32_t m_num_merged_static_methods = 0;
  uint32_t m_num_merged_direct_methods = 0;
  uint32_t m_num_merged_nonvirt_methods = 0;

  ModelStats& operator+=(const ModelStats& stats);
};
