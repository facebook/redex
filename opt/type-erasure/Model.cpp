/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include <set>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "AnnoUtils.h"
#include "ApproximateShapeMerging.h"
#include "ClassAssemblingUtils.h"
#include "DexStoreUtil.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Walkers.h"

size_t Model::s_shape_count = 0;
size_t Model::s_dex_count = 0;
size_t Model::s_num_interdex_groups = 0;
std::unordered_map<DexType*, size_t> Model::s_cls_to_interdex_group;

namespace {

constexpr const char* CLASS_MARKER_DELIMITER = "DexEndMarker";

std::string to_string(const ModelSpec& spec) {
  std::ostringstream ss;
  ss << spec.name << "(roots: ";
  for (const auto root : spec.roots) {
    ss << SHOW(root);
  }
  ss << ", exclude: " << spec.exclude_types.size()
     << ", prefix: " << spec.class_name_prefix
     << ", gen roots: " << spec.gen_types.size() << ")";
  return ss.str();
}

void load_generated_types(const ModelSpec& spec,
                          const Scope& scope,
                          const TypeSystem& type_system,
                          const TypeSet& models,
                          TypeSet& generated) {
  generated.insert(models.begin(), models.end());
  for (const auto& type : spec.gen_types) {
    const auto& cls = type_class(type);
    redex_assert(cls != nullptr);
    generated.insert(type);
    if (is_interface(cls)) {
      const auto& impls = type_system.get_implementors(type);
      generated.insert(impls.begin(), impls.end());
    } else {
      type_system.get_all_children(type, generated);
    }
  }
  for (const auto type : scope) {
    if (has_any_annotation(type, spec.gen_annos)) {
      generated.insert(type->get_type());
    }
  }
}

/**
 * Return true if 'left' is a subset of 'right'.
 * That is every element in left is in right as well.
 */
template <class Set>
bool is_subset(const Set& left, const Set& right) {
  for (const auto& el : left) {
    if (right.count(el) == 0) return false;
  }
  return true;
}

void print_interface_maps(const TypeToTypeSet& intf_to_classes,
                          const TypeSet& types) {
  std::vector<const DexType*> intfs;
  for (const auto& intf_to_classes_it : intf_to_classes) {
    intfs.emplace_back(intf_to_classes_it.first);
  }
  std::sort(intfs.begin(), intfs.end(),
            [&](const DexType* first, const DexType* second) {
              return intf_to_classes.at(first).size() <
                     intf_to_classes.at(second).size();
            });
  for (const auto& intf : intfs) {
    const auto& classes = intf_to_classes.at(intf);
    TRACE(TERA, 8, "- interface %s -> %ld\n", SHOW(intf), classes.size());
    if (classes.size() <= 5) {
      for (const auto& cls : classes) {
        TRACE(TERA, 8, "\t-(%ld) %s\n", types.count(cls), SHOW(cls));
      }
    }
  }
}

/**
 * trim shapes that have the mergeable type count less or
 * equal to ModelSpec.min_count
 */
size_t trim_shapes(MergerType::ShapeCollector& shapes, size_t min_count) {
  size_t num_trimmed_types = 0;
  std::vector<MergerType::Shape> shapes_to_remove;
  for (const auto& shape_it : shapes) {
    if (shape_it.second.types.size() > min_count) {
      TRACE(TERA, 7, "Keep shape %s (%ld)\n",
            shape_it.first.to_string().c_str(), shape_it.second.types.size());
      continue;
    }
    shapes_to_remove.push_back(shape_it.first);
  }
  for (const auto& shape : shapes_to_remove) {
    TRACE(TERA, 7, "Drop shape %s (%ld)\n", shape.to_string().c_str(),
          shapes[shape].types.size());
    num_trimmed_types += shapes[shape].types.size();
    shapes.erase(shape);
  }
  return num_trimmed_types;
}

/**
 * trim groups that have the mergeable types count less or
 * equal to ModelSpec.min_count.
 */
size_t trim_groups(MergerType::ShapeCollector& shapes, size_t min_count) {
  size_t num_trimmed_types = 0;
  TRACE(TERA, 5, "Trim groups with min_count %d\n", min_count);
  for (auto& shape_it : shapes) {
    std::vector<TypeSet> groups_to_remove;
    for (const auto& group_it : shape_it.second.groups) {
      if (group_it.second.size() > min_count) {
        TRACE(TERA, 7, "Keep group (%ld) on %s\n", group_it.second.size(),
              shape_it.first.to_string().c_str());
        continue;
      }
      groups_to_remove.push_back(group_it.first);
    }
    for (const auto& group : groups_to_remove) {
      auto& types = shape_it.second.groups[group];
      TRACE(TERA, 7, "Drop group (%ld) on %s\n", types.size(),
            shape_it.first.to_string().c_str());
      num_trimmed_types += types.size();
      for (const auto& type : types) {
        shape_it.second.types.erase(type);
      }
      shape_it.second.groups.erase(group);
    }
  }

  return num_trimmed_types;
}

const std::vector<std::string> get_acceptible_prefixes(const JsonWrapper& jw) {
  std::vector<std::string> acceptible_prefixes;
  jw.get("acceptible_prefixes", {}, acceptible_prefixes);
  return acceptible_prefixes;
}

bool is_android_sdk_type(const std::string& android_sdk_prefix,
                         const std::vector<std::string>& acceptible_prefixes,
                         const DexType* type) {
  const std::string& name = type->str();
  for (const auto& acceptible_prefix : acceptible_prefixes) {
    if (boost::starts_with(name, acceptible_prefix)) {
      return false;
    }
  }
  return boost::starts_with(name, android_sdk_prefix);
}

void exclude_reference_to_android_sdk(const Json::Value& json_val,
                                      const TypeSet& mergeables,
                                      TypeSet& non_mergeables) {
  JsonWrapper spec = JsonWrapper(json_val);
  bool enabled = false;
  spec.get("enabled", false, enabled);
  if (!enabled) {
    TRACE(TERA, 5, "Non mergeable (android_sdk) not enabled\n");
    return;
  }

  std::string android_sdk_prefix = "Landroid/";
  TRACE(TERA, 5, "Non mergeable (android_sdk) android_sdk_prefix %s\n",
        android_sdk_prefix.c_str());
  const std::vector<std::string> acceptible_prefixes =
      get_acceptible_prefixes(spec);

  std::vector<DexClass*> mergeable_classes;
  for (const auto t : mergeables) {
    auto cls = type_class(t);
    mergeable_classes.push_back(cls);
  }
  // Check field references
  walk::fields(mergeable_classes, [&](DexField* field) {
    auto type = field->get_type();
    if (is_android_sdk_type(android_sdk_prefix, acceptible_prefixes, type)) {
      auto mergeable = field->get_class();
      TRACE(TERA, 5, "Non mergeable (android_sdk) %s referencing %s\n",
            SHOW(mergeable), SHOW(type));
      non_mergeables.emplace(mergeable);
    }
  });

  // Scan code references.
  auto scanner = [&](DexMethod* meth) {
    std::unordered_set<const DexType*> current_excluded;
    auto code = meth->get_code();
    if (!code) {
      return current_excluded;
    }

    auto mergeable = meth->get_class();
    for (const auto& mie : InstructionIterable(code)) {
      std::vector<DexType*> gathered;
      mie.insn->gather_types(gathered);
      for (const auto referenced_type : gathered) {
        if (is_android_sdk_type(android_sdk_prefix, acceptible_prefixes,
                                referenced_type)) {
          TRACE(TERA, 5, "Non mergeable (android_sdk) %s referencing %s\n",
                SHOW(mergeable), SHOW(referenced_type));
          current_excluded.insert(mergeable);
        }
      }
    }

    return current_excluded;
  };
  auto excluded_by_android_sdk_ref =
      walk::parallel::reduce_methods<std::unordered_set<const DexType*>>(
          mergeable_classes,
          scanner,
          [](std::unordered_set<const DexType*> left,
             const std::unordered_set<const DexType*> right) {
            left.insert(right.begin(), right.end());
            return left;
          });
  for (const auto excluded : excluded_by_android_sdk_ref) {
    non_mergeables.insert(excluded);
  }
}

} // namespace

const TypeSet Model::empty_set = TypeSet();

Model::Model(const Scope& scope,
             const ModelSpec& spec,
             const TypeSystem& type_system,
             const TypeSet& types)
    : m_spec(spec), m_types(types), m_type_system(type_system), m_scope(scope) {
  init(scope, spec, type_system);
}

Model::Model(const Scope& scope,
             const DexStoresVector& stores,
             const ModelSpec& spec,
             const TypeSystem& type_system,
             ConfigFiles& conf)
    : m_spec(spec), m_type_system(type_system), m_scope(scope) {
  for (const auto root : spec.roots) {
    m_type_system.get_all_children(root, m_types);
  }
  init(scope, spec, type_system, &conf);
  find_non_root_store_mergeables(stores, spec.include_primary_dex);
}

void Model::init(const Scope& scope,
                 const ModelSpec& spec,
                 const TypeSystem& type_system,
                 ConfigFiles* conf) {
  build_hierarchy(spec.roots);
  for (const auto root : spec.roots) {
    build_interface_map(root, {});
  }
  print_interface_maps(m_intf_to_classes, m_types);

  for (const auto root : spec.roots) {
    MergerType* root_merger = build_mergers(root);
    m_roots.push_back(root_merger);
  }

  // load all generated types and find non mergeables
  TypeSet generated;
  load_generated_types(spec, scope, type_system, m_types, generated);
  TRACE(TERA, 4, "Generated types %ld\n", generated.size());
  exclude_types(spec.exclude_types);
  find_non_mergeables(scope, generated);
  m_metric.all_types = m_types.size();
}

void Model::build_hierarchy(const TypeSet& roots) {
  for (const auto& type : m_types) {
    if (roots.count(type) > 0) {
      continue;
    }
    const auto cls = type_class(type);
    const auto super = cls->get_super_class();
    redex_assert(super != nullptr && super != get_object_type());
    m_hierarchy[super].insert(type);
    m_parents[type] = super;
  }
}

void Model::build_interface_map(const DexType* type, TypeSet implemented) {
  TypeSet class_intfs = m_type_system.get_implemented_interfaces(type);
  for (const auto& impl : implemented) {
    class_intfs.erase(impl);
  }
  if (!class_intfs.empty()) {
    for (const auto intf : class_intfs) {
      m_class_to_intfs[type].insert(intf);
      m_intf_to_classes[intf].insert(type);
    }
    implemented.insert(class_intfs.begin(), class_intfs.end());
  }
  const auto& children = m_hierarchy.find(type);
  if (children != m_hierarchy.end()) {
    for (const auto& child : children->second) {
      build_interface_map(child, implemented);
    }
  }
}

MergerType* Model::build_mergers(const DexType* root) {
  auto& merger = create_dummy_merger(root);
  const auto& children = m_hierarchy.find(root);
  if (children != m_hierarchy.end()) {
    for (const auto& child : children->second) {
      create_dummy_mergers_if_children(child);
    }
  }
  return &merger;
}

void Model::build_interdex_groups(ConfigFiles* conf) {
  if (!conf) {
    return;
  }

  const auto& interdex_order = conf->get_coldstart_classes();
  if (interdex_order.size() == 0) {
    // No grouping based on interdex.
    s_num_interdex_groups = 0;
    return;
  }

  size_t group_id = 0;
  for (auto it = interdex_order.begin(); it != interdex_order.end(); ++it) {
    const auto& cls_name = *it;
    bool is_marker_delim =
        cls_name.find(CLASS_MARKER_DELIMITER) != std::string::npos;

    if (is_marker_delim || std::next(it) == interdex_order.end()) {
      group_id++;

      if (is_marker_delim) {
        continue;
      }
    }

    DexType* type = DexType::get_type(cls_name);
    if (type && s_cls_to_interdex_group.count(type) == 0) {
      s_cls_to_interdex_group[type] = group_id;
    }
  }

  // group_id + 1 represents the number of groups (considering the classes
  // outside of the interdex order as a group on its own).
  s_num_interdex_groups = group_id + 1;
}

MergerType& Model::create_dummy_merger(const DexType* type) {
  auto& merger = m_mergers[type];
  merger.type = type;
  merger.dummy = true;
  return merger;
}

void Model::create_dummy_mergers_if_children(const DexType* type) {
  if (m_excluded.count(type) > 0) {
    return;
  }
  if (m_non_mergeables.count(type) > 0) {
    return;
  }
  const auto& children = m_hierarchy.find(type);
  if (children == m_hierarchy.end()) {
    return;
  }
  create_dummy_merger(type);
  for (const auto& child : children->second) {
    create_dummy_mergers_if_children(child);
  }
}

MergerType& Model::create_merger_shape(const DexType* shape_type,
                                       const MergerType::Shape& shape,
                                       const DexType* parent,
                                       const TypeSet& intfs,
                                       const TypeSet& classes) {
  TRACE(TERA, 7, "Create Shape %s - %s, parent %s, intfs %ld, classes %ld\n",
        SHOW(shape_type), shape.to_string().c_str(), SHOW(parent), intfs.size(),
        classes.size());
  auto& merger = m_mergers[shape_type];
  merger.type = shape_type;
  merger.shape = shape;
  merger.from_shape = true;

  always_assert(classes.size() > 1);
  for (const auto& cls : classes) {
    // add the class to the mergeables of this shape
    merger.mergeables.insert(cls);

    // update interface<->class maps
    for (const auto& intf : intfs) {
      auto erased = m_intf_to_classes[intf].erase(cls);
      always_assert(erased > 0);
      erased = m_class_to_intfs[cls].erase(intf);
      always_assert(erased > 0);
    }
    always_assert(m_class_to_intfs[cls].empty());
    m_class_to_intfs.erase(cls);

    // update the parent<->child relationship
    const auto& cls_parent = m_parents.find(cls);
    always_assert(cls_parent != m_parents.end());
    auto erased = m_hierarchy[cls_parent->second].erase(cls);
    always_assert(erased > 0);
    if (m_hierarchy[cls_parent->second].empty()) {
      erased = m_hierarchy.erase(cls_parent->second);
      always_assert(erased > 0);
    }
    m_parents.erase(cls_parent);
  }

  // set up type system info for the shape
  set_parent_child(parent, shape_type);
  for (const auto& intf : intfs) {
    m_intf_to_classes[intf].insert(shape_type);
    m_class_to_intfs[shape_type].insert(intf);
  }

  return merger;
}

MergerType& Model::create_merger_helper(
    const DexType* merger_type,
    const MergerType::Shape& shape,
    const TypeSet& group_key,
    const TypeSet& group_values,
    const boost::optional<size_t>& dex_num,
    const boost::optional<size_t>& interdex_subgroup_idx,
    const boost::optional<size_t>& subgroup_idx) {
  size_t group_count = m_shape_to_count[shape]++;
  std::string name = shape.build_type_name(
      m_spec.class_name_prefix, merger_type, std::string("Shape"), group_count,
      dex_num, interdex_subgroup_idx, subgroup_idx);
  const auto& shape_type = DexType::make_type(name.c_str());
  TRACE(TERA, 7, "Build shape type %s\n", SHOW(shape_type));
  auto& merger_shape = create_merger_shape(shape_type, shape, merger_type,
                                           group_key, group_values);
  merger_shape.interdex_subgroup = interdex_subgroup_idx;

  map_fields(merger_shape, group_values);
  return merger_shape;
}

void Model::create_mergers_helper(
    const DexType* merger_type,
    const MergerType::Shape& shape,
    const TypeSet& group_key,
    const TypeSet& group_values,
    const boost::optional<size_t>& dex_num,
    const boost::optional<size_t>& interdex_subgroup_idx,
    const boost::optional<size_t>& max_mergeables_count) {
  size_t group_size = group_values.size();

  if (max_mergeables_count && group_size > *max_mergeables_count) {
    TypeSet curr_group;
    size_t subgroup_cnt = 0;
    size_t remaining_mergeable_cnt = group_size;
    for (auto it = group_values.begin(); it != group_values.end(); ++it) {
      auto mergeable = *it;
      curr_group.insert(mergeable);
      if ((curr_group.size() == *max_mergeables_count &&
           remaining_mergeable_cnt - *max_mergeables_count > 1) ||
          std::next(it) == group_values.end()) {
        create_merger_helper(merger_type, shape, group_key, curr_group, dex_num,
                             interdex_subgroup_idx,
                             boost::optional<size_t>(subgroup_cnt++));
        remaining_mergeable_cnt -= curr_group.size();
        curr_group.clear();
      }
    }
    always_assert(curr_group.empty());
  } else {
    create_merger_helper(merger_type, shape, group_key, group_values, dex_num,
                         interdex_subgroup_idx, boost::none);
  }
}

/**
 * Excluding the types specified in the "exclude" option of the config.
 * We don't perform any checks on the given types. We simply assume the good
 * intention of adding them as excluded types in the config, and exclude them
 * from the merging transformation.
 */
void Model::exclude_types(const std::unordered_set<DexType*>& exclude_types) {
  for (const auto& type : exclude_types) {
    const auto& cls = type_class(type);
    redex_assert(cls != nullptr);
    if (is_interface(cls)) {
      const auto& impls = m_type_system.get_implementors(type);
      m_excluded.insert(impls.begin(), impls.end());
    } else {
      m_excluded.insert(type);
      m_type_system.get_all_children(type, m_excluded);
    }
  }
  m_metric.excluded = m_excluded.size();
  TRACE(TERA, 4, "Excluded %ld\n", m_excluded.size());
}

/**
 * Try to identify types referenced by operations that Type Erasure does not
 * support. Such operations include reflections, instanceof checks on
 * no-type-tag shapes.
 * Ideally, part of the checks we perform below should be enforced at Java
 * source level. That is we should restrict such use cases on the generated Java
 * classes. As a result, we can make those generated classes easier to optimize
 * by Type Erasure.
 */
void Model::find_non_mergeables(const Scope& scope, const TypeSet& generated) {
  for (const auto& type : m_types) {
    const auto& cls = type_class(type);
    if (!can_delete(cls)) {
      m_non_mergeables.insert(type);
      TRACE(TERA, 5, "Cannot delete %s\n", SHOW(type));
    }
  }
  TRACE(TERA, 4, "Non mergeables (no delete) %ld\n", m_non_mergeables.size());

  bool has_type_tag = m_spec.has_type_tag();
  const auto& const_generated = generated;
  const auto& const_types = m_types;
  auto patcher = [has_type_tag, &const_generated,
                  &const_types](DexMethod* meth) {
    TypeSet current_non_mergeables;
    auto code = meth->get_code();
    if (!code || const_generated.count(meth->get_class())) {
      return current_non_mergeables;
    }

    for (const auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;

      // Java language level enforcement recommended!
      //
      // For mergeables with type tags, it is not safe to merge those used
      // with CONST_CLASS or NEW_ARRAY since we will lose granularity as we
      // can't map to the old type anymore.
      if (has_type_tag && insn->opcode() != OPCODE_CONST_CLASS &&
          insn->opcode() != OPCODE_NEW_ARRAY) {
        continue;
      }

      // Java language level enforcement recommended!
      //
      // For mergeables without a type tag, it is not safe to merge
      // those used in an INSTANCE_OF, since we might lose granularity.
      //
      // Example where both <type_0> and <type_1> have the same shape (so end
      //        up in the same merger)
      //
      //    INSTANCE_OF <v_result>, <v_obj> <type_0>
      //    then label:
      //      CHECK_CAST <type_0>
      //    else labe:
      //      CHECK_CAST <type_1>
      if (!has_type_tag && insn->opcode() != OPCODE_INSTANCE_OF) {
        continue;
      }

      const auto& type = get_array_type_or_self(insn->get_type());
      if (const_types.count(type) > 0) {
        current_non_mergeables.insert(type);
      }
    }

    return current_non_mergeables;
  };

  TypeSet non_mergeables_opcode = walk::parallel::reduce_methods<TypeSet>(
      scope, patcher, [](TypeSet left, const TypeSet right) {
        left.insert(right.begin(), right.end());
        return left;
      });

  m_non_mergeables.insert(non_mergeables_opcode.begin(),
                          non_mergeables_opcode.end());

  TRACE(TERA, 4, "Non mergeables (opcodes) %ld\n", m_non_mergeables.size());

  static DexType* string_type = get_string_type();

  if (!m_spec.merge_types_with_static_fields) {
    walk::fields(scope, [&](DexField* field) {
      if (generated.count(field->get_class()) > 0) {
        if (is_static(field)) {
          auto rtype = get_array_type_or_self(field->get_type());
          if (!is_primitive(rtype) && rtype != string_type) {
            // If the type is either non-primitive or a list of
            // non-primitive types (excluding Strings), then exclude it as
            // we might change the initialization order.
            TRACE(TERA,
                  5,
                  "[non mergeable] %s as it contains a non-primitive "
                  "static field\n",
                  SHOW(field->get_class()));
            m_non_mergeables.emplace(field->get_class());
          }
        }
      }
    });
  }

  if (!m_spec.exclude_reference_to_android_sdk.isNull()) {
    exclude_reference_to_android_sdk(m_spec.exclude_reference_to_android_sdk,
                                     m_types, m_non_mergeables);
  }

  m_metric.non_mergeables = m_non_mergeables.size();
  TRACE(TERA, 3, "Non mergeables %ld\n", m_non_mergeables.size());
}

void Model::find_non_root_store_mergeables(const DexStoresVector& stores,
                                           bool include_primary_dex) {
  std::unordered_set<const DexType*> non_root_store_types =
      get_non_root_store_types(stores, m_types, include_primary_dex);

  for (const DexType* type : non_root_store_types) {
    m_non_mergeables.insert(type);
  }
}

/**
 * Aggregate all mergeable types under a merger according to their shape.
 * Create a merger for every shape and move the mergeable types under
 * that shape.
 * Example:
 * class A { int f1; String f2; }
 * class B { int f1; Object f2; }
 * class C { int[] f1; int f2; }
 * The 3 classes above have all the same shape:
 * class Shape { Object field0; int field1; }
 */
void Model::shape_model() {
  std::vector<MergerType*> mergers;
  // sort mergers before creating the shapes.
  std::set<const DexType*, dextypes_comparator> merger_types;
  for (auto& merger_it : m_mergers) {
    merger_types.emplace(merger_it.first);
  }

  for (const auto& type : merger_types) {
    mergers.emplace_back(&m_mergers[type]);
  }

  for (auto merger : mergers) {
    TRACE(TERA, 6, "Build shapes from %s\n", SHOW(merger->type));
    MergerType::ShapeCollector shapes;
    shape_merger(*merger, shapes);
    approximate_shapes(shapes);
    m_metric.dropped += trim_shapes(shapes, m_spec.min_count);
    for (auto& shape_it : shapes) {
      break_by_interface(*merger, shape_it.first, shape_it.second);
    }

    flatten_shapes(*merger, shapes);
  }
}

void Model::shape_merger(const MergerType& merger,
                         MergerType::ShapeCollector& shapes) {
  // if the root has got no children there is nothing to "shape"
  const auto& children = m_hierarchy.find(merger.type);
  if (children == m_hierarchy.end()) return;

  // build a map from shape to types with that shape
  for (const auto& child : children->second) {
    if (m_hierarchy.find(child) != m_hierarchy.end()) continue;
    if (m_excluded.count(child)) {
      continue;
    }
    if (m_non_mergeables.count(child)) {
      continue;
    }

    const auto& cls = type_class(child);
    if (cls == nullptr) continue;

    MergerType::Shape shape{0, 0, 0, 0, 0, 0, 0};
    for (const auto& field : cls->get_ifields()) {
      const auto field_type = field->get_type();
      if (field_type == get_string_type()) {
        shape.string_fields++;
        continue;
      }
      switch (type_shorty(field_type)) {
      case 'L':
      case '[':
        shape.reference_fields++;
        break;
      case 'J':
        shape.long_fields++;
        break;
      case 'D':
        shape.double_fields++;
        break;
      case 'F':
        shape.float_fields++;
        break;
      case 'Z':
        shape.bool_fields++;
        break;
      case 'B':
      case 'S':
      case 'C':
      case 'I':
        shape.int_fields++;
        break;
      default:
        always_assert(false);
        break;
      }
    }

    TRACE(TERA, 9, "Shape of %s [%ld]: %s\n", SHOW(child),
          cls->get_ifields().size(), shape.to_string().c_str());

    shapes[shape].types.insert(child);
  }
}

/**
 * Depending the spec, choosing a approximation algorithm to merge different
 * shapes together. By default, no approximation is done.
 */
void Model::approximate_shapes(MergerType::ShapeCollector& shapes) {
  if (m_spec.approximate_shape_merging.isNull()) {
    TRACE(TERA, 3, "[approx] No approximate shape merging specified.\n");
    return;
  }

  JsonWrapper approx_spec = JsonWrapper(m_spec.approximate_shape_merging);
  std::string algo_name;
  approx_spec.get("algorithm", "", algo_name);

  // List shapes before approximation
  size_t num_total_mergeable = 0;
  size_t num_before_shapes = 0;
  TRACE(TERA, 3, "[approx] Shapes before approximation:\n");
  for (const auto& s : shapes) {
    TRACE(TERA, 3, "         Shape: %s, mergeables = %ld\n",
          s.first.to_string().c_str(), s.second.types.size());
    num_before_shapes++;
    num_total_mergeable += s.second.types.size();
  }
  TRACE(TERA, 3, "[approx] Total shapes before approximation = %zu\n",
        num_before_shapes);

  if (num_total_mergeable == 0) {
    return;
  }

  always_assert(s_outdir != "");
  TRACE(TERA, 3, "[approx] output dir is: %s\n", s_outdir.c_str());

  // Select an approximation algorithm
  if (algo_name == "simple_greedy") {
    simple_greedy_approximation(approx_spec, shapes, m_approx_stats);
  } else if (algo_name == "max_mergeable_greedy") {
    max_mergeable_greedy(approx_spec, s_outdir, shapes, m_approx_stats);
  } else if (algo_name == "max_shape_merged_greedy") {
    max_shape_merged_greedy(approx_spec, s_outdir, shapes, m_approx_stats);
  } else {
    TRACE(TERA, 3,
          "[approx] Invalid approximate shape merging spec, skipping...\n");
    return;
  }

  // List shapes after approximation
  size_t num_after_shapes = 0;
  TRACE(TERA, 3, "[approx] Shapes after approximation:\n");
  for (const auto& s_pair : shapes) {
    TRACE(TERA, 3, "         Shape: %s, mergeables = %ld\n",
          s_pair.first.to_string().c_str(), s_pair.second.types.size());
    num_after_shapes++;
    num_total_mergeable -= s_pair.second.types.size();
  }
  always_assert(num_total_mergeable == 0);
  TRACE(TERA, 3, "[approx] Total shapes after approximation = %zu\n",
        num_after_shapes);
}

/**
 * Break up a set of types by their interfaces implemention.
 * This step is critical to keep the type system "happy".
 */
void Model::break_by_interface(const MergerType& merger,
                               const MergerType::Shape& shape,
                               MergerType::ShapeHierarchy& hier) {
  always_assert(!hier.types.empty());
  // group classes by interfaces implemented
  TRACE(TERA, 7, "Break up shape %s parent %s\n", shape.to_string().c_str(),
        SHOW(merger.type));
  for (const auto& type : hier.types) {
    const auto& cls_intfs = m_class_to_intfs.find(type);
    if (cls_intfs == m_class_to_intfs.end()) {
      hier.groups[Model::empty_set].insert(type);
      continue;
    }
    auto& intfs = cls_intfs->second;
    hier.groups[intfs].insert(type);
  }
  TRACE(TERA, 7, "%ld groups created for shape %s (%ld)\n", hier.groups.size(),
        shape.to_string().c_str(), hier.types.size());
}

namespace {

DexType* check_current_instance(const TypeSet& types, IRInstruction* insn) {
  DexType* type = nullptr;
  if (insn->has_type()) {
    type = insn->get_type();
  } else if (insn->has_method()) {
    type = insn->get_method()->get_class();
  } else if (insn->has_field()) {
    type = insn->get_field()->get_class();
  }

  if (type == nullptr || types.count(type) == 0) {
    return nullptr;
  }

  return type;
}

std::unordered_map<DexType*, std::unordered_set<DexType*>> get_type_usages(
    const TypeSet& types, const Scope& scope) {
  std::unordered_map<DexType*, std::unordered_set<DexType*>> res;

  walk::opcodes(
      scope, [](DexMethod*) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        auto current_instance = check_current_instance(types, insn);
        if (current_instance) {
          res[current_instance].emplace(method->get_class());
        }

        if (insn->has_method()) {
          auto callee =
              resolve_method(insn->get_method(), opcode_to_search(insn));
          if (!callee) {
            return;
          }
          auto proto = callee->get_proto();
          auto rtype = proto->get_rtype();
          if (rtype && types.count(rtype)) {
            res[rtype].emplace(method->get_class());
          }

          for (const auto& type : proto->get_args()->get_type_list()) {
            if (type && types.count(type)) {
              res[type].emplace(method->get_class());
            }
          }
        }
      });

  return res;
}

size_t get_interdex_group(
    const std::unordered_set<DexType*>& types,
    const std::unordered_map<DexType*, size_t>& cls_to_interdex_groups,
    size_t interdex_groups) {
  // By default, we consider the class in the last group.
  size_t group = interdex_groups - 1;
  for (DexType* type : types) {
    if (cls_to_interdex_groups.count(type)) {
      group = std::min(group, cls_to_interdex_groups.at(type));
    }
  }

  return group;
}

} // namespace

std::vector<TypeSet> Model::group_per_interdex_set(const TypeSet& types) {
  auto type_to_usages = get_type_usages(types, m_scope);
  std::vector<TypeSet> new_groups(s_num_interdex_groups);
  for (const auto& pair : type_to_usages) {
    auto index = get_interdex_group(pair.second, s_cls_to_interdex_group,
                                    s_num_interdex_groups);
    new_groups[index].emplace(pair.first);
  }

  if (m_spec.merge_per_interdex_set == InterDexGroupingType::NON_HOT_SET) {
    // Drop mergeables that are in the hot set.
    new_groups[0].clear();
  }

  return new_groups;
}

void Model::flatten_shapes(const MergerType& merger,
                           MergerType::ShapeCollector& shapes) {
  size_t num_trimmed_types = trim_groups(shapes, m_spec.min_count);
  m_metric.dropped += num_trimmed_types;
  // sort shapes by mergeables count
  std::vector<const MergerType::Shape*> keys;
  for (auto& shape_it : shapes) {
    keys.emplace_back(&shape_it.first);
  }
  std::sort(
      keys.begin(), keys.end(),
      [&](const MergerType::Shape* first, const MergerType::Shape* second) {
        return shapes[*first].types.size() > shapes[*second].types.size();
      });

  // create shapes
  for (const auto& shape : keys) {
    const auto& shape_model = shapes[*shape];

    std::vector<const TypeSet*> group_keys;
    for (const auto& group_it : shape_model.groups) {
      group_keys.emplace_back(&group_it.first);
    }

    // sort groups by mergeables count
    std::sort(group_keys.begin(),
              group_keys.end(),
              [&](const TypeSet* left, const TypeSet* right) {
                auto& left_group = shape_model.groups.at(*left);
                auto& right_group = shape_model.groups.at(*right);

                if (left_group.size() == right_group.size()) {
                  const DexType* left_first_type = *left_group.begin();
                  const DexType* right_first_type = *right_group.begin();

                  return compare_dextypes(left_first_type, right_first_type);
                }

                return left_group.size() > right_group.size();
              });

    boost::optional<size_t> dex_num = is_dex_sharding_enabled()
                                          ? boost::optional<size_t>(s_dex_count)
                                          : boost::none;

    bool merge_per_interdex_set = is_merge_per_interdex_set_enabled();

    for (const TypeSet* group_key : group_keys) {
      const TypeSet& group_values = shape_model.groups.at(*group_key);
      if (merge_per_interdex_set && s_num_interdex_groups > 1) {
        auto new_groups = group_per_interdex_set(group_values);

        for (size_t gindex = 0; gindex < new_groups.size(); gindex++) {
          if (new_groups[gindex].size() < 1 ||
              new_groups[gindex].size() < m_spec.min_count) {
            continue;
          }

          create_mergers_helper(merger.type, *shape, *group_key,
                                new_groups[gindex], boost::none, gindex,
                                m_spec.max_count);
        }
      } else {
        create_mergers_helper(merger.type, *shape, *group_key, group_values,
                              dex_num, boost::none, m_spec.max_count);
      }
    }
  }

  if (is_dex_sharding_enabled()) {
    // Account for the current dex we generated shapes for.
    s_dex_count++;
  }
}

void Model::map_fields(MergerType& merger, const TypeSet& classes) {
  TRACE(TERA, 8, "Build field map for %s\n", SHOW(merger.type));
  always_assert(merger.is_shape());
  if (merger.field_count() == 0) return;
  // for each mergeable type build order the field accroding to the
  // shape. The field order shape is implicit and defined by the shape itself
  for (const auto& type : classes) {
    TRACE(TERA, 8, "Collecting fields for %s\n", SHOW(type));
    std::vector<DexField*> fields(merger.field_count());
    const DexClass* cls = type_class(type);
    for (const auto& field : cls->get_ifields()) {
      size_t index = merger.start_index_for(field->get_type());
      for (; index < fields.size(); index++) {
        if (fields[index] != nullptr) {
          continue;
        }
        TRACE(TERA, 8, "Add field %s\n", show_deobfuscated(field).c_str());
        fields[index] = field;
        break;
      }
      always_assert(index < fields.size());
    }
    for (size_t index = 0; index < fields.size(); index++) {
      // If the fields array is not fully filled, this means the shape is larger
      // than the actual class (possibly due to approximate shape merging), we
      // make a new field as a placeholder.
      if (fields[index] != nullptr) {
        continue;
      }
      std::ostringstream ss;
      ss << "placeholder_" << index;
      auto field_type = merger.field_type_at(index);
      fields[index] = static_cast<DexField*>(DexField::make_field(
          type, DexString::make_string(ss.str()), field_type));
      TRACE(TERA, 9,
            "  -- A hole found at index %d, created a placeholder field of "
            "type %s\n",
            index, field_type->c_str());
    }
    TRACE(TERA, 8, "Add field map item [%ld]\n", fields.size());
    merger.field_map[type] = fields;
  }
}

/**
 * Build the method lists for a merger, collecting all methods that
 * belong to the mergeable types.
 */
void Model::collect_methods() {
  // collect all vmethods and dmethods of mergeable types into the merger
  for (auto& merger_it : m_mergers) {
    auto& merger = merger_it.second;
    if (merger.mergeables.empty()) continue;
    TRACE(TERA,
          8,
          "Collect methods for merger %s [%ld]\n",
          SHOW(merger.type),
          merger.mergeables.size());
    for (const auto& type : merger.mergeables) {
      const auto& cls = type_class(type);
      always_assert(cls);
      TRACE(TERA, 8, "Merge %s\n", SHOW(type));

      TRACE(TERA,
            8,
            "%ld dmethods in %s\n",
            cls->get_dmethods().size(),
            SHOW(cls->get_type()));
      for (const auto& method : cls->get_dmethods()) {
        merger.dmethods.emplace_back(method);
      }

      const auto& virt_scopes = m_type_system.get_class_scopes().get(type);
      TRACE(TERA,
            8,
            "%ld virtual scopes in %s\n",
            virt_scopes.size(),
            SHOW(type));
      for (const auto& virt_scope : virt_scopes) {

        // interface methods
        if (is_impl_scope(virt_scope)) {
          TRACE(TERA,
                8,
                "interface virtual scope [%ld]\n",
                virt_scope->methods.size());
          add_interface_scope(merger, *virt_scope);
          continue;
        }

        // non virtual methods
        if (is_non_virtual_scope(virt_scope)) {
          TRACE(TERA,
                8,
                "non virtual scope %s (%s)\n",
                virt_scope->methods[0].first->get_deobfuscated_name().c_str(),
                SHOW(virt_scope->methods[0].first->get_name()));
          merger.non_virt_methods.emplace_back(virt_scope->methods[0].first);
          continue;
        }

        // virtual methods
        add_virtual_scope(merger, *virt_scope);
      }
    }
  }

  // now for the virtual methods up the hierarchy and those in the type
  // of the merger (if an existing type) distribute them across the
  // proper merger
  // collect all virtual scope up the hierarchy from a root
  for (const MergerType* merger_root : m_roots) {
    std::vector<const VirtualScope*> base_scopes;
    const auto root_type = merger_root->type;
    // get the first existing type from roots (has a DexClass)
    auto cls = type_class(root_type);
    while (cls == nullptr) {
      const auto parent = m_parents.find(root_type);
      if (parent == m_parents.end()) break;
      cls = type_class(parent->second);
    }
    // load all parents scopes
    const auto& parents = m_type_system.parent_chain(cls->get_type());
    if (parents.size() > 1) {
      for (auto index = parents.size() - 1; index > 0; --index) {
        const auto type = parents[index - 1];
        for (const auto& virt_scope :
             m_type_system.get_class_scopes().get(type)) {
          base_scopes.emplace_back(virt_scope);
        }
      }
    }

    distribute_virtual_methods(merger_root->type, base_scopes);
  }
}

// virtual methods
void Model::add_virtual_scope(MergerType& merger,
                              const VirtualScope& virt_scope) {
  merger.vmethods.emplace_back(nullptr, std::vector<DexMethod*>());
  for (const auto& vmeth : virt_scope.methods) {
    TRACE(TERA, 9, "check virtual method %s\n", SHOW(vmeth.first));
    always_assert_log(vmeth.first->is_def(), "not def %s", SHOW(vmeth.first));
    if (merger.mergeables.count(vmeth.first->get_class()) == 0) continue;
    TRACE(TERA, 8, "add virtual method %s\n", SHOW(vmeth.first));
    merger.vmethods.back().second.emplace_back(vmeth.first);
  }
}

void Model::add_interface_scope(MergerType& merger,
                                const VirtualScope& intf_scope) {

  const auto& insert = [&](MergerType::InterfaceMethod& intf_meths) {
    intf_meths.interfaces.insert(intf_scope.interfaces.begin(),
                                 intf_scope.interfaces.end());
    for (const auto vmeth : intf_scope.methods) {
      if (!vmeth.first->is_def()) continue;
      if (merger.mergeables.count(vmeth.first->get_class()) == 0) continue;
      TRACE(TERA,
            8,
            "add interface method %s (%s)\n",
            vmeth.first->get_deobfuscated_name().c_str(),
            SHOW(vmeth.first->get_name()));
      intf_meths.methods.emplace_back(vmeth.first);
    }
  };

  always_assert(intf_scope.methods.size() > 0);
  const auto& vmethod = intf_scope.methods[0];
  for (auto& intf_meths : merger.intfs_methods) {
    if (signatures_match(intf_meths.methods[0], vmethod.first)) {
      insert(intf_meths);
      return;
    }
  }
  merger.intfs_methods.push_back(MergerType::InterfaceMethod());
  insert(merger.intfs_methods.back());
}

void Model::distribute_virtual_methods(
    const DexType* type, std::vector<const VirtualScope*> base_scopes) {
  TRACE(TERA,
        8,
        "distribute virtual methods for %s, parent virtual scope %ld\n",
        SHOW(type),
        base_scopes.size());
  // add to the base_scope the class scope of the merger type
  const auto& class_scopes = m_type_system.get_class_scopes();
  const auto& virt_scopes = class_scopes.get(type);
  for (const auto& virt_scope : virt_scopes) {
    if (virt_scope->methods.size() == 1) continue;
    TRACE(TERA,
          8,
          "virtual scope found [%ld] %s\n",
          virt_scope->methods.size(),
          SHOW(virt_scope->methods[0].first));
    base_scopes.emplace_back(virt_scope);
  }

  const auto& merger = m_mergers.find(type);
  if (merger != m_mergers.end() && !merger->second.mergeables.empty()) {
    TRACE(TERA, 8, "merger found %s\n", SHOW(merger->second.type));
    // loop through the parent scopes of the mergeable types and
    // if a method is from a mergeable type add it to the merger
    for (const auto& virt_scope : base_scopes) {
      TRACE(TERA,
            8,
            "walking virtual scope [%s, %ld] %s (%s)\n",
            SHOW(virt_scope->type),
            virt_scope->methods.size(),
            virt_scope->methods[0].first->get_deobfuscated_name().c_str(),
            SHOW(virt_scope->methods[0].first->get_name()));
      bool is_interface = !virt_scope->interfaces.empty();
      std::vector<DexMethod*>* insert_list = nullptr;
      // TODO(zwei): currently we only handle overridden method resided in the
      // based type. If we plan to support more complicated vertical hierarchy,
      // we need to revise the logic here.
      auto top_def = virt_scope->methods[0].first;
      DexMethod* overridden_meth = top_def->is_def() ? top_def : nullptr;
      for (const auto& vmeth : virt_scope->methods) {
        if (!vmeth.first->is_def()) {
          continue;
        }
        if (merger->second.mergeables.count(vmeth.first->get_class()) == 0) {
          continue;
        }
        TRACE(TERA,
              9,
              "method %s (%s)\n",
              vmeth.first->get_deobfuscated_name().c_str(),
              SHOW(vmeth.first->get_name()));
        if (is_interface) {
          if (insert_list == nullptr) {
            // must be a new method
            TRACE(TERA,
                  8,
                  "add interface method %s (%s) w/ overridden_meth %s\n",
                  vmeth.first->get_deobfuscated_name().c_str(),
                  SHOW(vmeth.first->get_name()),
                  SHOW(overridden_meth));
            merger->second.intfs_methods.push_back(
                MergerType::InterfaceMethod());
            auto& intf_meth = merger->second.intfs_methods.back();
            intf_meth.overridden_meth = overridden_meth;
            merger->second.intfs_methods.back().interfaces.insert(
                virt_scope->interfaces.begin(), virt_scope->interfaces.end());
            insert_list = &merger->second.intfs_methods.back().methods;
          }
          insert_list->emplace_back(vmeth.first);
        } else {
          if (insert_list == nullptr) {
            // must be a new method
            TRACE(TERA,
                  8,
                  "add virtual method %s w/ overridden_meth %s\n",
                  SHOW(vmeth.first),
                  SHOW(overridden_meth));
            merger->second.vmethods.emplace_back(overridden_meth,
                                                 std::vector<DexMethod*>());
            insert_list = &merger->second.vmethods.back().second;
          }
          insert_list->emplace_back(vmeth.first);
        }
      }
    }
  }

  // walk the children and keep distributing as needed
  const auto& children = m_hierarchy.find(type);
  if (children != m_hierarchy.end()) {
    for (const auto& child : children->second) {
      distribute_virtual_methods(child, base_scopes);
    }
  }
}

std::string Model::print() const {
  size_t count{0};
  for (const auto& merger : m_mergers) {
    count += merger.second.mergeables.size();
  }
  std::ostringstream ss;
  ss << m_spec.name << " Model: all types " << m_types.size()
     << ", merge types " << m_mergers.size() << ", mergeables " << count
     << "\n";
  for (const auto root_merger : m_roots) {
    ss << print(root_merger->type, 1);
  }
  return ss.str();
}

std::string Model::print(const MergerType* merger) const {
  std::ostringstream ss;
  ss << SHOW(merger->type) << " mergeables(" << merger->mergeables.size() << ")"
     << " shape(str: " << merger->shape.string_fields
     << ", refs: " << merger->shape.reference_fields
     << ", bool: " << merger->shape.bool_fields
     << ", int: " << merger->shape.int_fields
     << ", long: " << merger->shape.long_fields
     << ", double: " << merger->shape.double_fields
     << ", float: " << merger->shape.float_fields << ") dmethods("
     << merger->dmethods.size() << ") non_virt_methods("
     << merger->non_virt_methods.size() << ") vmethods("
     << merger->vmethods.size();
  for (const auto& meths : merger->vmethods) {
    ss << "[" << meths.second.size() << "]";
  }
  ss << ") intf_methods(" << merger->intfs_methods.size();
  for (const auto& intf_meths : merger->intfs_methods) {
    ss << "[" << intf_meths.methods.size() << "]";
  }
  ss << ")";
  const auto& children = m_hierarchy.find(merger->type);
  if (children != m_hierarchy.end()) {
    ss << " children(" << children->second.size() << ")";
  }
  const auto& intfs = m_class_to_intfs.find(merger->type);
  if (intfs != m_class_to_intfs.end()) {
    ss << " interfaces(" << intfs->second.size() << ")";
    if (intfs->second.size() <= 7) {
      for (const auto& intf : intfs->second) {
        ss << ", " << SHOW(intf);
      }
    }
  }
  return ss.str();
}

std::string Model::print(const DexType* type) const {
  std::ostringstream ss;
  ss << SHOW(type);
  const auto& children = m_hierarchy.find(type);
  if (children != m_hierarchy.end()) {
    ss << " children(" << children->second.size() << ")";
  }
  const auto& intfs = m_class_to_intfs.find(type);
  if (intfs != m_class_to_intfs.end()) {
    ss << " interfaces(" << intfs->second.size() << ")";
    size_t count = 0;
    for (const auto& intf : intfs->second) {
      if (count++ > 6) break;
      ss << ", " << SHOW(intf);
    }
  }
  return ss.str();
}

std::string Model::print(const DexType* type, int nest) const {
  std::ostringstream ss;

  const auto indent = [&](char ch) {
    for (int i = 0; i < nest; i++) {
      ss << ch;
    }
  };

  const auto& merger = m_mergers.find(type);
  indent('+');
  ss << " ";
  if (merger != m_mergers.end()) {
    ss << print(&merger->second);
  } else {
    ss << print(type);
  }
  ss << "\n";

  if (merger != m_mergers.end()) {
    for (const auto& mergeable : merger->second.mergeables) {
      indent('-');
      ss << " " << print(mergeable) << "\n";
      const auto cls = type_class(mergeable);
      for (const auto& field : cls->get_ifields()) {
        indent('-');
        ss << "* " << show_deobfuscated(field) << " ("
           << field->get_name()->c_str() << ")\n";
      }
    }

    const auto meth_str = [&](const DexMethod* meth) {
      indent('-');
      ss << "# " << show_deobfuscated(meth) << " (" << meth->get_name()->c_str()
         << ") [" << meth->get_code()->count_opcodes() << "]\n";
    };

    if (!merger->second.dmethods.empty()) {
      indent('-');
      ss << "# " << merger->second.dmethods.size() << " dmethods:\n";
      for (const auto& meth : merger->second.dmethods) {
        meth_str(meth);
      }
    }

    if (!merger->second.non_virt_methods.empty()) {
      indent('-');
      ss << "# " << merger->second.non_virt_methods.size()
         << " non virtual methods:\n";
      for (const auto& meth : merger->second.non_virt_methods) {
        meth_str(meth);
      }
    }

    if (!merger->second.vmethods.empty()) {
      indent('-');
      ss << "# " << merger->second.vmethods.size() << " virtual methods:\n";
      for (const auto& vmeths : merger->second.vmethods) {
        for (const auto& meth : vmeths.second) {
          meth_str(meth);
        }
      }
    }

    if (!merger->second.intfs_methods.empty()) {
      indent('-');
      ss << "# " << merger->second.intfs_methods.size()
         << " interface methods:\n";
      for (const auto& intf_meths : merger->second.intfs_methods) {
        for (const auto& meth : intf_meths.methods) {
          meth_str(meth);
        }
      }
    }
  }
  const auto& children = m_hierarchy.find(type);
  if (children != m_hierarchy.end()) {
    for (const auto& child : children->second) {
      ss << print(child, nest + 1);
    }
  }
  return ss.str();
}

void Model::update_model(Model& model) {
  TRACE(TERA, 3, "Shape Model\n");
  model.shape_model();
  TRACE(TERA, 3, "Model:\n%s\nShape Model done\n", model.print().c_str());

  TRACE(TERA, 2, "Final Model\n");
  model.collect_methods();
  TRACE(TERA, 2, "Model:\n%s\nFinal Model done\n", model.print().c_str());
}

Model Model::build_model(const Scope& scope,
                         const DexStoresVector& stores,
                         const ModelSpec& spec,
                         ConfigFiles& conf) {
  Timer t("build_model");
  TypeSystem type_system(scope);

  TRACE(TERA, 3, "Build Model for %s\n", to_string(spec).c_str());
  Model model(scope, stores, spec, type_system, conf);
  TRACE(TERA, 3, "Model:\n%s\nBuild Model done\n", model.print().c_str());

  update_model(model);
  return model;
}

void Model::update_redex_stats(PassManager& mgr) const {
  mgr.incr_metric((m_spec.class_name_prefix + "_all_types").c_str(),
                  m_metric.all_types);
  mgr.incr_metric((m_spec.class_name_prefix + "_non_mergeables").c_str(),
                  m_metric.non_mergeables);
  mgr.incr_metric((m_spec.class_name_prefix + "_excluded_types").c_str(),
                  m_metric.excluded);
  mgr.incr_metric((m_spec.class_name_prefix + "_dropped_types").c_str(),
                  m_metric.dropped);

  if (!m_spec.approximate_shape_merging.isNull()) {
    mgr.incr_metric(
        (m_spec.class_name_prefix + "_approx_shapes_merged").c_str(),
        m_approx_stats.shapes_merged);
    mgr.incr_metric((m_spec.class_name_prefix + "_approx_mergeables").c_str(),
                    m_approx_stats.mergeables);
    mgr.incr_metric((m_spec.class_name_prefix + "_approx_fields_added").c_str(),
                    m_approx_stats.fields_added);
  }
}

Model Model::build_model(const Scope& scope,
                         const ModelSpec& spec,
                         const TypeSet& types) {
  Timer t("build_model");
  TypeSystem type_system(scope);

  TRACE(TERA, 3, "Build Model for %s\n", to_string(spec).c_str());
  Model model(scope, spec, type_system, types);
  TRACE(TERA, 3, "Model:\n%s\nBuild Model done\n", model.print().c_str());

  update_model(model);
  return model;
}

ModelStats& ModelStats::operator+=(const ModelStats& stats) {
  m_num_classes_merged += stats.m_num_classes_merged;
  m_num_generated_classes += stats.m_num_generated_classes;
  m_num_ctor_dedupped += stats.m_num_ctor_dedupped;
  m_num_static_non_virt_dedupped += stats.m_num_static_non_virt_dedupped;
  m_num_vmethods_dedupped += stats.m_num_vmethods_dedupped;
  m_num_const_lifted_methods += stats.m_num_const_lifted_methods;
  m_num_merged_static_methods += stats.m_num_merged_static_methods;
  m_num_merged_direct_methods += stats.m_num_merged_direct_methods;
  m_num_merged_nonvirt_methods += stats.m_num_merged_nonvirt_methods;
  return *this;
}
