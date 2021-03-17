/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include "AnnoUtils.h"
#include "ApproximateShapeMerging.h"
#include "ConfigFiles.h"
#include "MergeabilityCheck.h"
#include "MergingStrategies.h"
#include "PassManager.h"
#include "RefChecker.h"
#include "Resolver.h"
#include "Show.h"
#include "Walkers.h"

using namespace class_merging;

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
  if (!spec.gen_annos.empty()) {
    for (const auto type : scope) {
      if (has_any_annotation(type, spec.gen_annos)) {
        generated.insert(type->get_type());
      }
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
    TRACE(CLMG, 8, "- interface %s -> %ld", SHOW(intf), classes.size());
    if (classes.size() <= 5) {
      for (const auto& cls : classes) {
        TRACE(CLMG, 8, "\t-(%ld) %s", types.count(cls), SHOW(cls));
      }
    }
  }
}

/**
 * trim shapes that have the mergeable type count less than ModelSpec.min_count
 */
size_t trim_shapes(MergerType::ShapeCollector& shapes, size_t min_count) {
  size_t num_trimmed_types = 0;
  std::vector<MergerType::Shape> shapes_to_remove;
  for (const auto& shape_it : shapes) {
    if (shape_it.second.types.size() >= min_count) {
      TRACE(CLMG, 7, "Keep shape %s (%ld)", shape_it.first.to_string().c_str(),
            shape_it.second.types.size());
      continue;
    }
    shapes_to_remove.push_back(shape_it.first);
  }
  for (const auto& shape : shapes_to_remove) {
    TRACE(CLMG, 7, "Drop shape %s (%ld)", shape.to_string().c_str(),
          shapes[shape].types.size());
    num_trimmed_types += shapes[shape].types.size();
    shapes.erase(shape);
  }
  return num_trimmed_types;
}

/**
 * trim groups that have the mergeable types count less than ModelSpec.min_count
 */
size_t trim_groups(MergerType::ShapeCollector& shapes, size_t min_count) {
  size_t num_trimmed_types = 0;
  TRACE(CLMG, 5, "Trim groups with min_count %d", min_count);
  for (auto& shape_it : shapes) {
    std::vector<TypeSet> groups_to_remove;
    for (const auto& group_it : shape_it.second.groups) {
      if (group_it.second.size() >= min_count) {
        TRACE(CLMG, 7, "Keep group (%ld) on %s", group_it.second.size(),
              shape_it.first.to_string().c_str());
        continue;
      }
      groups_to_remove.push_back(group_it.first);
    }
    for (const auto& group : groups_to_remove) {
      auto& types = shape_it.second.groups[group];
      TRACE(CLMG, 7, "Drop group (%ld) on %s", types.size(),
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

} // namespace

const TypeSet Model::empty_set = TypeSet();

Model::Model(const Scope& scope,
             const ConfigFiles& conf,
             const DexStoresVector& stores,
             const ModelSpec& spec,
             const TypeSystem& type_system,
             const RefChecker& refchecker)
    : m_spec(spec),
      m_type_system(type_system),
      m_ref_checker(refchecker),
      m_scope(scope),
      m_conf(conf) {
  for (const auto root : spec.roots) {
    m_type_system.get_all_children(root, m_types);
  }
  init(scope, spec, type_system);
}

void Model::init(const Scope& scope,
                 const ModelSpec& spec,
                 const TypeSystem& type_system) {
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
  TRACE(CLMG, 4, "Generated types %ld", generated.size());
  exclude_types(spec.exclude_types);
  // find_non_mergeables(scope, generated);
  MergeabilityChecker checker(scope, spec, m_ref_checker, generated, m_types);
  m_non_mergeables = checker.get_non_mergeables();
  TRACE(CLMG, 3, "Non mergeables %ld", m_non_mergeables.size());
  m_metric.non_mergeables = m_non_mergeables.size();
  m_metric.all_types = m_types.size();
}

void Model::build_hierarchy(const TypeSet& roots) {
  for (const auto& type : m_types) {
    if (roots.count(type) > 0) {
      continue;
    }
    const auto cls = type_class(type);
    const auto super = cls->get_super_class();
    redex_assert(super != nullptr && super != type::java_lang_Object());
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

void Model::build_interdex_groups(ConfigFiles& conf) {
  const auto& interdex_order = conf.get_coldstart_classes();
  if (interdex_order.empty()) {
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
  if (is_excluded(type)) {
    m_excluded.insert(type);
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

MergerType& Model::create_merger_shape(
    const DexType* shape_type,
    const MergerType::Shape& shape,
    const DexType* parent,
    const TypeSet& intfs,
    const std::vector<const DexType*>& classes) {
  TRACE(CLMG, 7, "Create Shape %s - %s, parent %s, intfs %ld, classes %ld",
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
    const TypeSet& intf_set,
    const std::vector<const DexType*>& group_values,
    const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
    const InterdexSubgroupIdx subgroup_idx) {
  size_t group_count = m_shape_to_count[shape]++;
  std::string name = shape.build_type_name(
      m_spec.class_name_prefix, merger_type, std::string("Shape"), group_count,
      interdex_subgroup_idx, subgroup_idx);
  const auto& shape_type = DexType::make_type(name.c_str());
  TRACE(CLMG, 7, "Build shape type %s", SHOW(shape_type));
  auto& merger_shape = create_merger_shape(shape_type, shape, merger_type,
                                           intf_set, group_values);
  merger_shape.interdex_subgroup = interdex_subgroup_idx;

  map_fields(merger_shape, group_values);
  return merger_shape;
}

void Model::create_mergers_helper(
    const DexType* merger_type,
    const MergerType::Shape& shape,
    const TypeSet& intf_set,
    const TypeSet& group_values,
    const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
    const boost::optional<size_t>& max_mergeables_count,
    size_t min_mergeables_count) {
  InterdexSubgroupIdx subgroup_cnt = 0;
  strategy::split_groups(
      group_values, min_mergeables_count, max_mergeables_count,
      [&](const std::vector<const DexType*>& group) {
        create_merger_helper(merger_type, shape, intf_set, group,
                             interdex_subgroup_idx, subgroup_cnt++);
      });
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
  TRACE(CLMG, 4, "Excluding types %ld", m_excluded.size());
}

bool Model::is_excluded(const DexType* type) const {
  if (m_excluded.count(type)) {
    return true;
  }
  for (const auto& prefix : m_spec.exclude_prefixes) {
    if (boost::starts_with(type->get_name()->str(), prefix)) {
      return true;
    }
  }
  return false;
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
  // sort mergers before creating the shapes.
  std::vector<MergerType*> mergers;
  mergers.reserve(m_mergers.size());
  for (auto& merger_it : m_mergers) {
    mergers.emplace_back(&merger_it.second);
  }
  std::sort(mergers.begin(), mergers.end(),
            [](const MergerType* first, const MergerType* second) {
              return compare_dextypes(first->type, second->type);
            });

  for (auto merger : mergers) {
    TRACE(CLMG, 6, "Build shapes from %s", SHOW(merger->type));
    MergerType::ShapeCollector shapes;
    shape_merger(*merger, shapes);
    approximate_shapes(shapes);

    m_metric.dropped += trim_shapes(shapes, m_spec.min_count);
    for (auto& shape_it : shapes) {
      break_by_interface(*merger, shape_it.first, shape_it.second);
    }

    flatten_shapes(*merger, shapes);
  }

  // Update excluded metrics
  m_metric.excluded = m_excluded.size();
  TRACE(CLMG, 4, "Excluded types total %ld", m_excluded.size());
}

void Model::shape_merger(const MergerType& merger,
                         MergerType::ShapeCollector& shapes) {
  // if the root has got no children there is nothing to "shape"
  const auto& children = m_hierarchy.find(merger.type);
  if (children == m_hierarchy.end()) {
    return;
  }

  // build a map from shape to types with that shape
  for (const auto& child : children->second) {
    if (m_hierarchy.find(child) != m_hierarchy.end()) {
      continue;
    }
    if (is_excluded(child)) {
      m_excluded.insert(child);
      continue;
    }
    if (m_non_mergeables.count(child)) {
      continue;
    }
    const auto& cls = type_class(child);
    if (cls == nullptr) {
      continue;
    }

    MergerType::Shape shape(cls->get_ifields());

    TRACE(CLMG, 9, "Shape of %s [%ld]: %s", SHOW(child),
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
    TRACE(CLMG, 3, "[approx] No approximate shape merging specified.");
    return;
  }

  JsonWrapper approx_spec = JsonWrapper(m_spec.approximate_shape_merging);
  std::string algo_name;
  approx_spec.get("algorithm", "", algo_name);

  // List shapes before approximation
  size_t num_total_mergeable = 0;
  size_t num_before_shapes = 0;
  TRACE(CLMG, 3, "[approx] Shapes before approximation:");
  for (const auto& s : shapes) {
    TRACE(CLMG, 3, "         Shape: %s, mergeables = %ld",
          s.first.to_string().c_str(), s.second.types.size());
    num_before_shapes++;
    num_total_mergeable += s.second.types.size();
  }
  TRACE(CLMG, 3, "[approx] Total shapes before approximation = %zu",
        num_before_shapes);

  if (num_total_mergeable == 0) {
    return;
  }

  // Select an approximation algorithm
  if (algo_name == "simple_greedy") {
    simple_greedy_approximation(approx_spec, shapes, m_approx_stats);
  } else if (algo_name == "max_mergeable_greedy") {
    max_mergeable_greedy(approx_spec, m_conf, shapes, m_approx_stats);
  } else if (algo_name == "max_shape_merged_greedy") {
    max_shape_merged_greedy(approx_spec, m_conf, shapes, m_approx_stats);
  } else {
    TRACE(CLMG, 3,
          "[approx] Invalid approximate shape merging spec, skipping...");
    return;
  }

  // List shapes after approximation
  size_t num_after_shapes = 0;
  TRACE(CLMG, 3, "[approx] Shapes after approximation:");
  for (const auto& s_pair : shapes) {
    TRACE(CLMG, 3, "         Shape: %s, mergeables = %ld",
          s_pair.first.to_string().c_str(), s_pair.second.types.size());
    num_after_shapes++;
    num_total_mergeable -= s_pair.second.types.size();
  }
  always_assert(num_total_mergeable == 0);
  TRACE(CLMG, 3, "[approx] Total shapes after approximation = %zu",
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
  TRACE(CLMG, 7, "Break up shape %s parent %s", shape.to_string().c_str(),
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
  TRACE(CLMG, 7, "%ld groups created for shape %s (%ld)", hier.groups.size(),
        shape.to_string().c_str(), hier.types.size());
}

namespace {

using TypeHashSet = std::unordered_set<DexType*>;
using ConstTypeHashSet = std::unordered_set<const DexType*>;

DexType* check_current_instance(const ConstTypeHashSet& types,
                                IRInstruction* insn) {
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

ConcurrentMap<DexType*, TypeHashSet> get_type_usages(
    const ConstTypeHashSet& types, const Scope& scope) {
  ConcurrentMap<DexType*, TypeHashSet> res;

  walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
    auto cls = method->get_class();
    const auto& updater =
        [&cls](DexType* /* key */, std::unordered_set<DexType*>& set,
               bool /* already_exists */) { set.emplace(cls); };

    auto current_instance = check_current_instance(types, insn);
    if (current_instance) {
      res.update(current_instance, updater);
    }

    if (insn->has_method()) {
      auto callee =
          resolve_method(insn->get_method(), opcode_to_search(insn), method);
      if (!callee) {
        return;
      }
      auto proto = callee->get_proto();
      auto rtype = proto->get_rtype();
      if (rtype && types.count(rtype)) {
        res.update(rtype, updater);
      }

      for (const auto& type : proto->get_args()->get_type_list()) {
        if (type && types.count(type)) {
          res.update(type, updater);
        }
      }
    }
  });

  return res;
}

size_t get_interdex_group(
    const TypeHashSet& types,
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

namespace class_merging {

std::vector<TypeSet> Model::group_per_interdex_set(const TypeSet& types) {
  ConstTypeHashSet type_hash_set{types.begin(), types.end()};
  const auto& type_to_usages = get_type_usages(type_hash_set, m_scope);
  std::vector<TypeSet> new_groups(s_num_interdex_groups);
  for (const auto& pair : type_to_usages) {
    auto index = get_interdex_group(pair.second, s_cls_to_interdex_group,
                                    s_num_interdex_groups);
    if (m_spec.merge_per_interdex_set == InterDexGroupingType::NON_HOT_SET) {
      if (index == 0) {
        // Drop mergeables that are in the hot set.
        continue;
      }
    } else if (m_spec.merge_per_interdex_set ==
               InterDexGroupingType::NON_ORDERED_SET) {
      if (index < s_num_interdex_groups - 1) {
        // Only merge the last group which are not in ordered set, drop other
        // mergeables.
        continue;
      }
    }
    new_groups[index].emplace(pair.first);
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
    const auto& shape_hierarchy = shapes[*shape];

    std::vector<const TypeSet*> intf_sets;
    for (const auto& group_it : shape_hierarchy.groups) {
      intf_sets.emplace_back(&group_it.first);
    }

    // sort groups by mergeables count
    std::sort(intf_sets.begin(),
              intf_sets.end(),
              [&](const TypeSet* left, const TypeSet* right) {
                auto& left_group = shape_hierarchy.groups.at(*left);
                auto& right_group = shape_hierarchy.groups.at(*right);

                if (left_group.size() == right_group.size()) {
                  const DexType* left_first_type = *left_group.begin();
                  const DexType* right_first_type = *right_group.begin();

                  return compare_dextypes(left_first_type, right_first_type);
                }

                return left_group.size() > right_group.size();
              });

    bool merge_per_interdex_set = is_merge_per_interdex_set_enabled();

    for (const TypeSet* intf_set : intf_sets) {
      const TypeSet& group_values = shape_hierarchy.groups.at(*intf_set);
      if (merge_per_interdex_set && s_num_interdex_groups > 1) {
        auto new_groups = group_per_interdex_set(group_values);

        redex_assert(new_groups.size() <
                     std::numeric_limits<InterdexSubgroupIdx>::max());
        for (InterdexSubgroupIdx gindex = 0; gindex < new_groups.size();
             gindex++) {
          if (new_groups[gindex].empty() ||
              new_groups[gindex].size() < m_spec.min_count) {
            continue;
          }

          create_mergers_helper(merger.type, *shape, *intf_set,
                                new_groups[gindex], gindex, m_spec.max_count,
                                m_spec.min_count);
        }
      } else {
        create_mergers_helper(merger.type, *shape, *intf_set, group_values,
                              boost::none, m_spec.max_count, m_spec.min_count);
      }
    }
  }
}

void Model::map_fields(MergerType& merger,
                       const std::vector<const DexType*>& classes) {
  TRACE(CLMG, 8, "Build field map for %s", SHOW(merger.type));
  always_assert(merger.is_shape());
  if (merger.field_count() == 0) return;
  // for each mergeable type build order the field accroding to the
  // shape. The field order shape is implicit and defined by the shape itself
  for (const auto& type : classes) {
    TRACE(CLMG, 8, "Collecting fields for %s", SHOW(type));
    std::vector<DexField*> fields(merger.field_count());
    const DexClass* cls = type_class(type);
    for (const auto& field : cls->get_ifields()) {
      size_t index = merger.start_index_for(field->get_type());
      for (; index < fields.size(); index++) {
        if (fields[index] != nullptr) {
          continue;
        }
        TRACE(CLMG, 8, "Add field %s", show_deobfuscated(field).c_str());
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
      TRACE(CLMG, 9,
            "  -- A hole found at index %d, created a placeholder field of "
            "type %s",
            index, field_type->c_str());
    }
    TRACE(CLMG, 8, "Add field map item [%ld]", fields.size());
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
    TRACE(CLMG,
          8,
          "Collect methods for merger %s [%ld]",
          SHOW(merger.type),
          merger.mergeables.size());
    for (const auto& type : merger.mergeables) {
      const auto& cls = type_class(type);
      always_assert(cls);
      TRACE(CLMG, 8, "Merge %s", SHOW(type));

      TRACE(CLMG,
            8,
            "%ld dmethods in %s",
            cls->get_dmethods().size(),
            SHOW(cls->get_type()));
      bool has_ctor = false;
      for (const auto& method : cls->get_dmethods()) {
        if (method::is_init(method)) {
          has_ctor = true;
        }
        merger.dmethods.emplace_back(method);
      }
      always_assert_log(has_ctor,
                        "[ClassMerging] No ctor found for mergeable %s",
                        SHOW(type));

      const auto& virt_scopes = m_type_system.get_class_scopes().get(type);
      TRACE(CLMG, 8, "%ld virtual scopes in %s", virt_scopes.size(),
            SHOW(type));
      for (const auto& virt_scope : virt_scopes) {

        // interface methods
        if (is_impl_scope(virt_scope)) {
          TRACE(CLMG,
                8,
                "interface virtual scope [%ld]",
                virt_scope->methods.size());
          add_interface_scope(merger, *virt_scope);
          continue;
        }

        // non virtual methods
        if (is_non_virtual_scope(virt_scope)) {
          TRACE(CLMG,
                8,
                "non virtual scope %s (%s)",
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
    TRACE(CLMG, 9, "check virtual method %s", SHOW(vmeth.first));
    always_assert_log(vmeth.first->is_def(), "not def %s", SHOW(vmeth.first));
    if (merger.mergeables.count(vmeth.first->get_class()) == 0) continue;
    TRACE(CLMG, 8, "add virtual method %s", SHOW(vmeth.first));
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
      TRACE(CLMG,
            8,
            "add interface method %s (%s)",
            vmeth.first->get_deobfuscated_name().c_str(),
            SHOW(vmeth.first->get_name()));
      intf_meths.methods.emplace_back(vmeth.first);
    }
  };

  always_assert(!intf_scope.methods.empty());
  const auto& vmethod = intf_scope.methods[0];
  for (auto& intf_meths : merger.intfs_methods) {
    if (method::signatures_match(intf_meths.methods[0], vmethod.first)) {
      insert(intf_meths);
      return;
    }
  }
  merger.intfs_methods.push_back(MergerType::InterfaceMethod());
  insert(merger.intfs_methods.back());
}

void Model::distribute_virtual_methods(
    const DexType* type, std::vector<const VirtualScope*> base_scopes) {
  TRACE(CLMG,
        8,
        "distribute virtual methods for %s, parent virtual scope %ld",
        SHOW(type),
        base_scopes.size());
  // add to the base_scope the class scope of the merger type
  const auto& class_scopes = m_type_system.get_class_scopes();
  const auto& virt_scopes = class_scopes.get(type);
  for (const auto& virt_scope : virt_scopes) {
    if (virt_scope->methods.size() == 1) continue;
    TRACE(CLMG,
          8,
          "virtual scope found [%ld] %s",
          virt_scope->methods.size(),
          SHOW(virt_scope->methods[0].first));
    base_scopes.emplace_back(virt_scope);
  }

  const auto& merger = m_mergers.find(type);
  if (merger != m_mergers.end() && !merger->second.mergeables.empty()) {
    TRACE(CLMG, 8, "merger found %s", SHOW(merger->second.type));
    // loop through the parent scopes of the mergeable types and
    // if a method is from a mergeable type add it to the merger
    for (const auto& virt_scope : base_scopes) {
      TRACE(CLMG,
            8,
            "walking virtual scope [%s, %ld] %s (%s)",
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
        TRACE(CLMG,
              9,
              "method %s (%s)",
              vmeth.first->get_deobfuscated_name().c_str(),
              SHOW(vmeth.first->get_name()));
        if (is_interface) {
          if (insert_list == nullptr) {
            // must be a new method
            TRACE(CLMG,
                  8,
                  "add interface method %s (%s) w/ overridden_meth %s",
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
            TRACE(CLMG,
                  8,
                  "add virtual method %s w/ overridden_meth %s",
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

std::string Model::show_type(const DexType* type) { return show(type); }

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

Model Model::build_model(const Scope& scope,
                         const ConfigFiles& conf,
                         const DexStoresVector& stores,
                         const ModelSpec& spec,
                         const TypeSystem& type_system,
                         const RefChecker& refchecker) {
  Timer t("build_model");

  TRACE(CLMG, 3, "Build Model for %s", to_string(spec).c_str());
  Model model(scope, conf, stores, spec, type_system, refchecker);
  TRACE(CLMG, 3, "Model:\n%s\nBuild Model done", model.print().c_str());

  TRACE(CLMG, 3, "Shape Model");
  model.shape_model();
  TRACE(CLMG, 3, "Model:\n%s\nShape Model done", model.print().c_str());

  TRACE(CLMG, 3, "Final Model");
  model.collect_methods();
  TRACE(CLMG, 3, "Model:\n%s\nFinal Model done", model.print().c_str());
  return model;
}

void Model::update_redex_stats(PassManager& mgr) const {
  mgr.incr_metric(m_spec.class_name_prefix + "_all_types", m_metric.all_types);
  mgr.incr_metric(m_spec.class_name_prefix + "_non_mergeables",
                  m_metric.non_mergeables);
  mgr.incr_metric(m_spec.class_name_prefix + "_excluded_types",
                  m_metric.excluded);
  mgr.incr_metric(m_spec.class_name_prefix + "_dropped_types",
                  m_metric.dropped);

  if (!m_spec.approximate_shape_merging.isNull()) {
    mgr.incr_metric(m_spec.class_name_prefix + "_approx_shapes_merged",
                    m_approx_stats.shapes_merged);
    mgr.incr_metric(m_spec.class_name_prefix + "_approx_mergeables",
                    m_approx_stats.mergeables);
    mgr.incr_metric(m_spec.class_name_prefix + "_approx_fields_added",
                    m_approx_stats.fields_added);
  }
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

} // namespace class_merging
