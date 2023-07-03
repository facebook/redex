/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include <ostream>
#include <sstream>
#include <string>

#include "AnnoUtils.h"
#include "ApproximateShapeMerging.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "MergeabilityCheck.h"
#include "MergingStrategies.h"
#include "PassManager.h"
#include "RefChecker.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
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
                          const ConstTypeHashSet& models,
                          TypeSet& generated) {
  if (spec.is_generated_code) {
    generated.insert(models.begin(), models.end());
  }
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
                          const ConstTypeHashSet& types) {
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
    TRACE(CLMG, 8, "- interface %s -> %zu", SHOW(intf), classes.size());
    if (classes.size() <= 5) {
      for (const auto& cls : classes) {
        TRACE(CLMG, 8, "\t-(%zu) %s", types.count(cls), SHOW(cls));
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
      TRACE(CLMG, 7, "Keep shape %s (%zu)", shape_it.first.to_string().c_str(),
            shape_it.second.types.size());
      continue;
    }
    shapes_to_remove.push_back(shape_it.first);
  }
  for (const auto& shape : shapes_to_remove) {
    TRACE(CLMG, 7, "Drop shape %s (%zu)", shape.to_string().c_str(),
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
  TRACE(CLMG, 5, "Trim groups with min_count %zu", min_count);
  for (auto& shape_it : shapes) {
    std::vector<TypeSet> groups_to_remove;
    for (const auto& group_it : shape_it.second.groups) {
      if (group_it.second.size() >= min_count) {
        TRACE(CLMG, 7, "Keep group (%zu) on %s", group_it.second.size(),
              shape_it.first.to_string().c_str());
        continue;
      }
      groups_to_remove.push_back(group_it.first);
    }
    for (const auto& group : groups_to_remove) {
      auto& types = shape_it.second.groups[group];
      TRACE(CLMG, 7, "Drop group (%zu) on %s", types.size(),
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
             const DexStoresVector& stores,
             const ConfigFiles& conf,
             const ModelSpec& spec,
             const TypeSystem& type_system,
             const RefChecker& refchecker)
    : m_spec(spec),
      m_type_system(type_system),
      m_ref_checker(refchecker),
      m_scope(scope),
      m_conf(conf),
      m_x_dex(XDexRefs(stores)) {
  init(scope, spec, type_system);
}

void Model::init(const Scope& scope,
                 const ModelSpec& spec,
                 const TypeSystem& type_system) {
  build_hierarchy(spec.roots);
  for (const auto root : spec.roots) {
    build_interface_map(root, {});
  }
  print_interface_maps(m_intf_to_classes, m_spec.merging_targets);

  for (const auto root : spec.roots) {
    MergerType* root_merger = build_mergers(root);
    m_roots.push_back(root_merger);
  }

  // load all generated types and find non mergeables
  TypeSet generated;
  load_generated_types(spec, scope, type_system, m_spec.merging_targets,
                       generated);
  TRACE(CLMG, 4, "Generated types %zu", generated.size());
  exclude_types(spec.exclude_types);
  MergeabilityChecker checker(scope, spec, m_ref_checker, generated);
  m_non_mergeables = checker.get_non_mergeables();
  TRACE(CLMG, 3, "Non mergeables %zu", m_non_mergeables.size());
  m_stats.m_non_mergeables = m_non_mergeables.size();
  m_stats.m_all_types = m_spec.merging_targets.size();
}

void Model::build_hierarchy(const TypeSet& roots) {
  for (const auto& type : m_spec.merging_targets) {
    if (roots.count(type) > 0) {
      continue;
    }
    const auto cls = type_class(type);
    const auto super = cls->get_super_class();
    redex_assert(super != nullptr);
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
  TRACE(CLMG, 7, "Create Shape %s - %s, parent %s, intfs %zu, classes %zu",
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
    const boost::optional<size_t>& dex_id,
    const ConstTypeVector& group_values,
    const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
    const InterdexSubgroupIdx subgroup_idx) {
  size_t group_count = m_shape_to_count[shape]++;
  std::string name = shape.build_type_name(
      m_spec.class_name_prefix, merger_type, intf_set, dex_id, group_count,
      interdex_subgroup_idx, subgroup_idx);
  const auto& shape_type = DexType::make_type(name);
  TRACE(CLMG, 7, "Build shape type %s", SHOW(shape_type));
  auto& merger_shape = create_merger_shape(shape_type, shape, merger_type,
                                           intf_set, group_values);
  merger_shape.dex_id = dex_id;
  merger_shape.interdex_subgroup = interdex_subgroup_idx;

  map_fields(merger_shape, group_values);
  return merger_shape;
}

void Model::create_mergers_helper(
    const DexType* merger_type,
    const MergerType::Shape& shape,
    const TypeSet& intf_set,
    const boost::optional<size_t>& dex_id,
    const TypeSet& group_values,
    const strategy::Strategy strategy,
    const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
    const boost::optional<size_t>& max_mergeables_count,
    size_t min_mergeables_count) {
  InterdexSubgroupIdx subgroup_cnt = 0;
  strategy::MergingStrategy ms(strategy, group_values);
  ms.apply_grouping(min_mergeables_count, max_mergeables_count,
                    [&](const ConstTypeVector& group) {
                      create_merger_helper(merger_type, shape, intf_set, dex_id,
                                           group, interdex_subgroup_idx,
                                           subgroup_cnt++);
                      m_stats.m_merging_size_counts[group.size()]++;
                    });
}

/**
 * Excluding the types specified in the "exclude" option of the config.
 * We don't perform any checks on the given types. We simply assume the good
 * intention of adding them as excluded types in the config, and exclude them
 * from the merging transformation.
 */
void Model::exclude_types(const ConstTypeHashSet& exclude_types) {
  for (const auto& type : exclude_types) {
    const auto& cls = type_class(type);
    if (cls == nullptr) {
      continue;
    }
    if (is_interface(cls)) {
      const auto& impls = m_type_system.get_implementors(type);
      m_excluded.insert(impls.begin(), impls.end());
    } else {
      m_excluded.insert(type);
      m_type_system.get_all_children(type, m_excluded);
    }
  }
  TRACE(CLMG, 4, "Excluding types %zu", m_excluded.size());
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

    m_stats.m_dropped += trim_shapes(shapes, m_spec.min_count);
    for (auto& shape_it : shapes) {
      break_by_interface(*merger, shape_it.first, shape_it.second);
    }

    flatten_shapes(*merger, shapes);
  }

  // Update excluded metrics
  m_stats.m_excluded = m_excluded.size();
  TRACE(CLMG, 4, "Excluded types total %zu", m_excluded.size());
}

void Model::shape_merger(const MergerType& root,
                         MergerType::ShapeCollector& shapes) {
  // if the root has got no children there is nothing to "shape"
  const auto& children = m_hierarchy.find(root.type);
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

    TRACE(CLMG, 9, "Shape of %s [%zu]: %s", SHOW(child),
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
    TRACE(CLMG, 3, "         Shape: %s, mergeables = %zu",
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
    simple_greedy_approximation(approx_spec, shapes, m_stats.m_approx_stats);
  } else if (algo_name == "max_mergeable_greedy") {
    max_mergeable_greedy(approx_spec, m_conf, shapes, m_stats.m_approx_stats);
  } else if (algo_name == "max_shape_merged_greedy") {
    max_shape_merged_greedy(approx_spec, m_conf, shapes,
                            m_stats.m_approx_stats);
  } else {
    TRACE(CLMG, 3,
          "[approx] Invalid approximate shape merging spec, skipping...");
    return;
  }

  // List shapes after approximation
  size_t num_after_shapes = 0;
  TRACE(CLMG, 3, "[approx] Shapes after approximation:");
  for (const auto& s_pair : shapes) {
    TRACE(CLMG, 3, "         Shape: %s, mergeables = %zu",
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
  TRACE(CLMG, 7, "%zu groups created for shape %s (%zu)", hier.groups.size(),
        shape.to_string().c_str(), hier.types.size());
}

namespace {

using TypeHashSet = std::unordered_set<DexType*>;

DexType* check_current_instance(const ConstTypeHashSet& types,
                                IRInstruction* insn) {
  DexType* type = nullptr;
  if (insn->has_type()) {
    type =
        const_cast<DexType*>(type::get_element_type_if_array(insn->get_type()));
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
    const ConstTypeHashSet& types,
    const Scope& scope,
    ModelSpec::InterDexGroupingInferringMode mode) {
  TRACE(CLMG, 1, "InterDex Grouping Inferring Mode %s",
        [&]() {
          std::ostringstream oss;
          oss << mode;
          return oss.str();
        }()
            .c_str());
  ConcurrentMap<DexType*, TypeHashSet> res;
  // Ensure all types will be handled.
  for (auto* t : types) {
    res.emplace(const_cast<DexType*>(t), TypeHashSet());
  }

  auto class_loads_update = [&](auto* insn, auto* cls) {
    const auto& updater =
        [&cls](DexType* /* key */, std::unordered_set<DexType*>& set,
               bool /* already_exists */) { set.emplace(cls); };

    if (insn->has_type()) {
      auto current_instance = check_current_instance(types, insn);
      if (current_instance) {
        res.update(current_instance, updater);
      }
    } else if (insn->has_field()) {
      if (opcode::is_an_sfield_op(insn->opcode())) {
        auto current_instance = check_current_instance(types, insn);
        if (current_instance) {
          res.update(current_instance, updater);
        }
      }
    } else if (insn->has_method()) {
      // Load and initialize class for static member access.
      if (opcode::is_invoke_static(insn->opcode())) {
        auto current_instance = check_current_instance(types, insn);
        if (current_instance) {
          res.update(current_instance, updater);
        }
      }
    }
  };

  switch (mode) {
  case ModelSpec::InterDexGroupingInferringMode::kAllTypeRefs: {
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

        for (const auto& type : *proto->get_args()) {
          if (type && types.count(type)) {
            res.update(type, updater);
          }
        }
      }
    });
    break;
  }

  case ModelSpec::InterDexGroupingInferringMode::kClassLoads: {
    walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
      auto cls = method->get_class();
      class_loads_update(insn, cls);
    });
    break;
  }

  case ModelSpec::InterDexGroupingInferringMode::
      kClassLoadsBasicBlockFiltering: {
    auto is_not_cold = [](cfg::Block* b) {
      auto* sb = source_blocks::get_first_source_block(b);
      if (sb == nullptr) {
        // Conservatively assume that missing SBs mean no profiling data.
        return true;
      }
      return sb->foreach_val_early(
          [](const auto& v) { return v && v->val > 0; });
    };
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      auto cls = method->get_class();

      cfg::ScopedCFG cfg{&code};

      for (auto* b : cfg->blocks()) {
        // TODO: If we split by interaction, we could check here specifically.
        if (is_not_cold(b)) {
          for (auto& mie : ir_list::InstructionIterable(b)) {
            class_loads_update(mie.insn, cls);
          }
        }
      }
    });
    break;
  }
  }

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

InterDexGroupingType get_merge_per_interdex_type(
    const std::string& interdex_grouping) {

  const static std::unordered_map<std::string, InterDexGroupingType>
      string_to_grouping = {
          {"disabled", InterDexGroupingType::DISABLED},
          {"non-hot-set", InterDexGroupingType::NON_HOT_SET},
          {"non-ordered-set", InterDexGroupingType::NON_ORDERED_SET},
          {"full", InterDexGroupingType::FULL}};

  always_assert_log(string_to_grouping.count(interdex_grouping) > 0,
                    "InterDex Grouping Type %s not found. Please check the list"
                    " of accepted values.",
                    interdex_grouping.c_str());
  return string_to_grouping.at(interdex_grouping);
}

std::ostream& operator<<(std::ostream& os,
                         ModelSpec::InterDexGroupingInferringMode mode) {
  switch (mode) {
  case ModelSpec::InterDexGroupingInferringMode::kAllTypeRefs:
    os << "all";
    break;
  case ModelSpec::InterDexGroupingInferringMode::kClassLoads:
    os << "class-loads";
    break;
  case ModelSpec::InterDexGroupingInferringMode::kClassLoadsBasicBlockFiltering:
    os << "class-loads-bb";
    break;
  }
  return os;
}

/**
 * Group merging targets according to their dex ids. Return a vector of dex_id
 * and types.
 */
TypeGroupByDex Model::group_per_dex(bool per_dex_grouping,
                                    const TypeSet& types) {
  if (!per_dex_grouping) {
    TypeSet group(types.begin(), types.end());
    return {{boost::none, group}};
  } else {
    std::vector<TypeSet> new_groups(m_x_dex.num_dexes());
    for (auto type : types) {
      auto dex_id = m_x_dex.get_dex_idx(type);
      new_groups[dex_id].emplace(type);
    }
    TypeGroupByDex result(m_x_dex.num_dexes());
    size_t dex_id = 0;
    for (auto&& group : new_groups) {
      result.emplace_back(std::make_pair(dex_id, std::move(group)));
      ++dex_id;
    }
    return result;
  }
}

TypeSet Model::get_types_in_current_interdex_group(
    const TypeSet& types, const ConstTypeHashSet& interdex_group_types) {
  TypeSet group;
  for (auto* type : types) {
    if (interdex_group_types.count(type)) {
      group.insert(type);
    }
  }
  return group;
}

/**
 * Split the types into groups according to the interdex grouping information.
 * Note that types may be dropped if they are not allowed be merged.
 */
std::vector<ConstTypeHashSet> Model::group_by_interdex_set(
    const ConstTypeHashSet& types) {
  size_t num_group = 1;
  if (is_interdex_grouping_enabled() && s_num_interdex_groups > 1) {
    num_group = s_num_interdex_groups;
  }
  std::vector<ConstTypeHashSet> new_groups(num_group);
  if (num_group == 1) {
    new_groups[0].insert(types.begin(), types.end());
    return new_groups;
  }
  const auto& type_to_usages =
      get_type_usages(types, m_scope, m_spec.interdex_grouping_inferring_mode);
  for (const auto& pair : type_to_usages) {
    auto index = get_interdex_group(pair.second, s_cls_to_interdex_group,
                                    s_num_interdex_groups);
    if (m_spec.interdex_grouping == InterDexGroupingType::NON_HOT_SET) {
      if (index == 0) {
        // Drop mergeables that are in the hot set.
        continue;
      }
    } else if (m_spec.interdex_grouping ==
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
  m_stats.m_dropped += num_trimmed_types;
  // Group all merging targets according to interdex grouping.
  auto all_interdex_groups = group_by_interdex_set(m_spec.merging_targets);
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

    for (const TypeSet* intf_set : intf_sets) {
      const TypeSet& implementors = shape_hierarchy.groups.at(*intf_set);
      for (auto& pair : group_per_dex(m_spec.per_dex_grouping, implementors)) {
        auto dex_id = pair.first;
        auto group_values = pair.second;
        if (all_interdex_groups.size() > 1) {
          for (InterdexSubgroupIdx interdex_gid = 0;
               interdex_gid < all_interdex_groups.size();
               interdex_gid++) {
            if (all_interdex_groups[interdex_gid].empty()) {
              continue;
            }
            auto new_group = get_types_in_current_interdex_group(
                group_values, all_interdex_groups[interdex_gid]);
            if (new_group.size() < m_spec.min_count) {
              continue;
            }
            create_mergers_helper(merger.type, *shape, *intf_set, dex_id,
                                  new_group, m_spec.strategy, interdex_gid,
                                  m_spec.max_count, m_spec.min_count);
            m_stats.m_interdex_groups[interdex_gid] += new_group.size();
          }
        } else {
          create_mergers_helper(merger.type, *shape, *intf_set, dex_id,
                                group_values, m_spec.strategy, boost::none,
                                m_spec.max_count, m_spec.min_count);
        }
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
            "  -- A hole found at index %zu, created a placeholder field of "
            "type %s",
            index, field_type->c_str());
    }
    TRACE(CLMG, 8, "Add field map item [%zu]", fields.size());
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
    if (merger.mergeables.empty()) {
      continue;
    }
    TRACE(CLMG,
          8,
          "Collect methods for merger %s [%zu]",
          SHOW(merger.type),
          merger.mergeables.size());
    for (const auto* mergeable : merger.mergeables) {
      const auto* cls = type_class(mergeable);
      always_assert(cls);
      TRACE(CLMG, 8, "  mergeable %s", SHOW(mergeable));
      TRACE(CLMG,
            8,
            "%zu dmethods in %s",
            cls->get_dmethods().size(),
            SHOW(cls->get_type()));
      bool has_ctor = false;
      for (auto* method : cls->get_dmethods()) {
        if (method::is_init(method)) {
          has_ctor = true;
        }
        merger.dmethods.emplace_back(method);
      }
      always_assert_log(has_ctor,
                        "[ClassMerging] No ctor found for mergeable %s",
                        SHOW(mergeable));

      const auto& virt_scopes = m_type_system.get_class_scopes().get(mergeable);
      TRACE(CLMG, 8, "%zu virtual scopes in %s", virt_scopes.size(),
            SHOW(mergeable));
      for (const auto* virt_scope : virt_scopes) {

        // interface methods
        if (is_impl_scope(virt_scope)) {
          TRACE(CLMG,
                8,
                "interface virtual scope [%zu]",
                virt_scope->methods.size());
          add_interface_scope(merger, *virt_scope);
          continue;
        }

        // non virtual methods
        if (is_non_virtual_scope(virt_scope)) {
          TRACE(CLMG,
                8,
                "non virtual scope %s (%s)",
                virt_scope->methods[0]
                    .first->get_deobfuscated_name_or_empty_copy()
                    .c_str(),
                SHOW(virt_scope->methods[0].first->get_name()));
          merger.non_virt_methods.emplace_back(virt_scope->methods[0].first);
          continue;
        }

        // virtual methods
        add_virtual_scope(merger, *virt_scope);
      }
    }

    for (auto& intf_meths : merger.intfs_methods) {
      if (intf_meths.methods.size() == merger.mergeables.size()) {
        // If the InterfaceMethod entry completely overrides the overridden one,
        // we don't need to track the overridden anymore. That way we don't
        // generate a call to the overridden fall back, since it won't be
        // reachable.
        intf_meths.overridden_meth = nullptr;
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
      if (parent == m_parents.end()) {
        break;
      }
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

/**
 * Add methods in `virt_scope` to a new MergerType.vmethods entry.
 * `virt_scope` is a VirtualScope rooted from a mergeable type of the current
 * `merger`.
 */
void Model::add_virtual_scope(MergerType& merger,
                              const VirtualScope& virt_scope) {
  // Add a new MergerType.vmethods entry w/o the base. This is just the
  // placeholder. VirtualMethod.base and VirtualMethod.overrides are to be
  // populated at a later point.
  merger.vmethods.emplace_back(nullptr);
  for (const auto& vmeth : virt_scope.methods) {
    TRACE(CLMG, 9, "check virtual method %s", SHOW(vmeth.first));
    always_assert_log(vmeth.first->is_def(), "not def %s", SHOW(vmeth.first));
    if (merger.mergeables.count(vmeth.first->get_class()) == 0) {
      continue;
    }
    TRACE(CLMG, 8, "add virtual method %s", SHOW(vmeth.first));
    merger.vmethods.back().overrides.emplace_back(vmeth.first);
  }
}

/**
 * Add methods in `virt_scope` to a new MergerType.intfs_methods entry.
 * `virt_scope` is a VirtualScope rooted from a mergeable type of the current
 * `merger`. `virt_scope` is also known to implement at least one interface
 * method.
 */
void Model::add_interface_scope(MergerType& merger,
                                const VirtualScope& intf_scope) {

  const auto& insert_to = [&merger, &intf_scope](
                              MergerType::InterfaceMethod& intf_meth) {
    bool inserted = false;
    intf_meth.interfaces.insert(intf_scope.interfaces.begin(),
                                intf_scope.interfaces.end());
    for (const auto& vmeth : intf_scope.methods) {
      // Only insert method defs
      if (!vmeth.first->is_def()) {
        continue;
      }
      // Only collect intf methods on mergeable types
      if (merger.mergeables.count(vmeth.first->get_class()) == 0) {
        continue;
      }
      TRACE(CLMG,
            8,
            "add interface method %s (%s)",
            vmeth.first->get_deobfuscated_name_or_empty_copy().c_str(),
            SHOW(vmeth.first->get_name()));
      intf_meth.methods.emplace_back(vmeth.first);
      inserted = true;
    }

    if (!inserted) {
      return;
    }

    // An interface VirtualScope rooted from a mergeable needs to consider
    // the interface method it implements. The interface method can be a
    // default method or even an external default method.
    // In the default method case, if it's not overridden by all mergeables,
    // we need to identify the default method as the fall back.
    // Here we check if the overridden interface is an external non-abstract
    // class. If it is, we assume it's an external default method, and
    // update MergerType.InterfaceMethod.overridden_method accordingly.
    if (!intf_meth.overridden_meth) {
      const auto& intfs = intf_scope.interfaces;
      always_assert(!intfs.empty());
      for (const auto* intf : intfs) {
        const auto* intf_cls = type_class(intf);
        always_assert(intf_cls);
        const auto* meth = intf_meth.methods.front();
        auto* intf_method = resolve_interface_method(intf_cls, meth->get_name(),
                                                     meth->get_proto());
        if (intf_method && !is_abstract(intf_method)) {
          intf_meth.overridden_meth = intf_method;
          TRACE(CLMG, 8, "Update InterfaceMethod.overridden_meth %s",
                SHOW(intf_method));
          break;
        }
      }
    }
  };

  always_assert(!intf_scope.methods.empty());
  const auto& vmethod = intf_scope.methods[0];
  for (auto& intf_meths : merger.intfs_methods) {
    if (method::signatures_match(intf_meths.methods[0], vmethod.first)) {
      insert_to(intf_meths);
      return;
    }
  }

  // No match for existing InterfaceMethods. Now create a new InterfaceMethod
  // and insert the current VirtualScope. But we only do so if the VirtualScope
  // has at least one method def.
  if (intf_scope.has_def()) {
    merger.intfs_methods.push_back(MergerType::InterfaceMethod());
    insert_to(merger.intfs_methods.back());
  }
}

void Model::distribute_virtual_methods(
    const DexType* type, std::vector<const VirtualScope*> base_scopes) {
  TRACE(CLMG,
        8,
        "distribute virtual methods for %s, parent virtual scope %zu",
        SHOW(type),
        base_scopes.size());
  // add to the base_scope the class scope of the merger type
  const auto& class_scopes = m_type_system.get_class_scopes();
  const auto& virt_scopes = class_scopes.get(type);
  for (const auto& virt_scope : virt_scopes) {
    if (virt_scope->methods.size() == 1) {
      continue;
    }
    TRACE(CLMG,
          8,
          "virtual scope found [%zu] %s",
          virt_scope->methods.size(),
          SHOW(virt_scope->methods[0].first));
    base_scopes.emplace_back(virt_scope);
  }

  const auto& merger_it = m_mergers.find(type);
  if (merger_it != m_mergers.end() && !merger_it->second.mergeables.empty()) {
    auto& merger = merger_it->second;
    TRACE(CLMG, 8, "merger found %s", SHOW(merger.type));
    // loop through the parent scopes of the mergeable types and
    // if a method is from a mergeable type add it to the merger
    for (const auto& virt_scope : base_scopes) {
      TRACE(CLMG,
            8,
            "walking virtual scope [%s, %zu] %s (%s)",
            SHOW(virt_scope->type),
            virt_scope->methods.size(),
            virt_scope->methods[0]
                .first->get_deobfuscated_name_or_empty_copy()
                .c_str(),
            SHOW(virt_scope->methods[0].first->get_name()));
      bool is_interface = !virt_scope->interfaces.empty();
      std::vector<DexMethod*>* insert_list = nullptr;
      // If the top_def is concrete, it's a valid virtual fallback for
      // mergeables w/o override. However, if the top_def is a non-def miranda,
      // we need to keep probing the next def in the same virtual scope. At the
      // same time, we need to make sure the overridden def we take is actually
      // on a base class of the targeted mergeables, not on a separate
      // inheritance branch.
      // We commit on the 1st valid base impl as the virtual fallback, not the
      // lowest one in the virtual scope. It's not necessary to go even lower,
      // the emitted code is correct. Virtual method refs can be rebound at a
      // later point.
      auto top_def = virt_scope->methods[0];
      DexMethod* overridden_meth =
          top_def.first->is_def() ? top_def.first : nullptr;
      const auto update_overridden = [&merger, &overridden_meth](
                                         const VirtualMethod& top_def,
                                         DexMethod* virt_meth) {
        always_assert(virt_meth->is_def());
        if (overridden_meth == nullptr && is_miranda(top_def.second)) {
          const auto* cls = virt_meth->get_class();
          const auto& mergeables = merger.mergeables;
          always_assert(!mergeables.empty());
          const auto* a_mergeable = *mergeables.begin();
          if (type::is_subclass(cls, a_mergeable)) {
            overridden_meth = virt_meth;
            TRACE(CLMG, 9, "Update overridden_meth to %s for top_def %s",
                  SHOW(virt_meth), SHOW(top_def.first));
          }
        }
      };

      for (const auto& pair : virt_scope->methods) {
        auto* virt_meth = pair.first;
        if (!virt_meth->is_def()) {
          continue;
        }
        if (merger.mergeables.count(virt_meth->get_class()) == 0) {
          update_overridden(virt_scope->methods[0], virt_meth);
          continue;
        }
        TRACE(CLMG,
              9,
              "method %s (%s)",
              virt_meth->get_deobfuscated_name_or_empty_copy().c_str(),
              SHOW(virt_meth->get_name()));
        if (is_interface) {
          if (insert_list == nullptr) {
            // must be a new method
            TRACE(CLMG,
                  8,
                  "add interface method %s (%s) w/ overridden_meth %s",
                  virt_meth->get_deobfuscated_name_or_empty_copy().c_str(),
                  SHOW(virt_meth->get_name()),
                  SHOW(overridden_meth));
            merger.intfs_methods.push_back(MergerType::InterfaceMethod());
            auto& intf_meth = merger.intfs_methods.back();
            intf_meth.overridden_meth = overridden_meth;
            merger.intfs_methods.back().interfaces.insert(
                virt_scope->interfaces.begin(), virt_scope->interfaces.end());
            insert_list = &merger.intfs_methods.back().methods;
          }
          insert_list->emplace_back(virt_meth);
        } else {
          if (insert_list == nullptr) {
            // must be a new method
            TRACE(CLMG,
                  8,
                  "add virtual method %s w/ overridden_meth %s",
                  SHOW(virt_meth),
                  SHOW(overridden_meth));
            merger.vmethods.emplace_back(overridden_meth);
            insert_list = &merger.vmethods.back().overrides;
          }
          insert_list->emplace_back(virt_meth);
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
  ss << m_spec.name << " Model: all types " << m_spec.merging_targets.size()
     << ", merge types " << m_mergers.size() << ", mergeables " << count
     << "\n";
  for (const auto root_merger : m_roots) {
    ss << print(root_merger->type, 1);
  }
  return ss.str();
}

std::string Model::print(const MergerType& merger) const {
  std::ostringstream ss;
  ss << SHOW(merger.type) << " mergeables(" << merger.mergeables.size() << ")"
     << " shape(str: " << merger.shape.string_fields
     << ", refs: " << merger.shape.reference_fields
     << ", bool: " << merger.shape.bool_fields
     << ", int: " << merger.shape.int_fields
     << ", long: " << merger.shape.long_fields
     << ", double: " << merger.shape.double_fields
     << ", float: " << merger.shape.float_fields << ") dmethods("
     << merger.dmethods.size() << ") non_virt_methods("
     << merger.non_virt_methods.size() << ") vmethods("
     << merger.vmethods.size();
  for (const auto& meths : merger.vmethods) {
    ss << "[" << meths.overrides.size() << "]";
  }
  ss << ") intf_methods(" << merger.intfs_methods.size();
  for (const auto& intf_meths : merger.intfs_methods) {
    ss << "[" << intf_meths.methods.size() << "]";
  }
  ss << ")";
  const auto& children = m_hierarchy.find(merger.type);
  if (children != m_hierarchy.end()) {
    ss << " children(" << children->second.size() << ")";
  }
  const auto& intfs = m_class_to_intfs.find(merger.type);
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

  const auto merger_it = m_mergers.find(type);
  indent('+');
  ss << " ";
  if (merger_it != m_mergers.end()) {
    const MergerType& merger = merger_it->second;
    ss << print(merger);
  } else {
    ss << print(type);
  }
  ss << "\n";

  if (merger_it != m_mergers.end()) {
    const MergerType& merger = merger_it->second;
    for (const auto& mergeable : merger.mergeables) {
      indent('-');
      ss << " " << print(mergeable) << "\n";
      const auto cls = type_class(mergeable);
      for (const auto& field : cls->get_ifields()) {
        indent('-');
        ss << "* " << show_deobfuscated(field) << " ("
           << field->get_name()->c_str() << ")\n";
      }
    }

    const auto meth_str = [&](const DexMethod* meth,
                              const std::string& suffix = "") {
      indent('-');
      if (meth) {
        ss << "# " << show_deobfuscated(meth) << " ("
           << meth->get_name()->c_str() << ") ["
           << (meth->get_code() ? meth->get_code()->count_opcodes() : 0) << "]";
      } else {
        ss << "# missing";
      }
      ss << (suffix.empty() ? "" : " (") << suffix << ")\n";
    };

    if (!merger_it->second.dmethods.empty()) {
      indent('-');
      ss << "# " << merger.dmethods.size() << " dmethods:\n";
      for (const auto* meth : merger.dmethods) {
        meth_str(meth);
      }
    }

    if (!merger_it->second.non_virt_methods.empty()) {
      indent('-');
      ss << "# " << merger.non_virt_methods.size() << " non virtual methods:\n";
      for (const auto* meth : merger_it->second.non_virt_methods) {
        meth_str(meth);
      }
    }

    if (!merger.vmethods.empty()) {
      indent('-');
      ss << "# " << merger.vmethods.size() << " virtual methods:\n";
      for (const auto& vmeths : merger.vmethods) {
        meth_str(vmeths.base, "base");
        for (const auto* meth : vmeths.overrides) {
          meth_str(meth);
        }
      }
    }

    if (!merger_it->second.intfs_methods.empty()) {
      indent('-');
      ss << "# " << merger.intfs_methods.size() << " interface methods:\n";
      for (const auto& intf_meths : merger.intfs_methods) {
        meth_str(intf_meths.overridden_meth, "overridden");
        for (const auto* meth : intf_meths.methods) {
          meth_str(meth);
        }
        if (merger.mergeables.size() > intf_meths.methods.size()) {
          if (!intf_meths.overridden_meth) {
            const auto& methods = intf_meths.methods;
            TRACE(CLMG, 8,
                  "interface method entry missing overridden method %s %zu",
                  SHOW(methods.front()), methods.size());
          }
        }
      }
    }
  }
  const auto& children = m_hierarchy.find(type);
  if (children != m_hierarchy.end()) {
    for (const auto* child : children->second) {
      ss << print(child, nest + 1);
    }
  }
  return ss.str();
}

Model Model::build_model(const Scope& scope,
                         const DexStoresVector& stores,
                         const ConfigFiles& conf,
                         const ModelSpec& spec,
                         const TypeSystem& type_system,
                         const RefChecker& refchecker) {
  Timer t("build_model");

  TRACE(CLMG, 3, "Build Model for %s", to_string(spec).c_str());
  Model model(scope, stores, conf, spec, type_system, refchecker);
  TRACE(CLMG, 3, "Model:\n%s\nBuild Model done", model.print().c_str());

  TRACE(CLMG, 3, "Shape Model");
  model.shape_model();
  TRACE(CLMG, 3, "Model:\n%s\nShape Model done", model.print().c_str());

  TRACE(CLMG, 3, "Final Model");
  model.collect_methods();
  TRACE(CLMG, 3, "Model:\n%s\nFinal Model done", model.print().c_str());

  return model;
}

ModelStats& ModelStats::operator+=(const ModelStats& stats) {
  m_all_types += stats.m_all_types;
  m_non_mergeables += stats.m_non_mergeables;
  m_excluded += stats.m_excluded;
  m_dropped += stats.m_dropped;

  for (const auto& pair : stats.m_interdex_groups) {
    m_interdex_groups[pair.first] += pair.second;
  }

  for (const auto& pair : stats.m_merging_size_counts) {
    m_merging_size_counts[pair.first] += pair.second;
  }

  m_approx_stats += stats.m_approx_stats;

  m_num_classes_merged += stats.m_num_classes_merged;
  m_num_generated_classes += stats.m_num_generated_classes;
  m_num_ctor_dedupped += stats.m_num_ctor_dedupped;
  m_num_static_non_virt_dedupped += stats.m_num_static_non_virt_dedupped;
  m_num_vmethods_dedupped += stats.m_num_vmethods_dedupped;
  m_num_const_lifted_methods += stats.m_num_const_lifted_methods;
  return *this;
}

void ModelStats::update_redex_stats(const std::string& prefix,
                                    PassManager& mgr) const {
  mgr.incr_metric(prefix + "_all_types", m_all_types);
  mgr.incr_metric(prefix + "_non_mergeables", m_non_mergeables);
  mgr.incr_metric(prefix + "_excluded_types", m_excluded);
  mgr.incr_metric(prefix + "_dropped_types", m_dropped);

  for (auto& pair : m_interdex_groups) {
    auto group_id = pair.first;
    auto group_size = pair.second;
    mgr.incr_metric(prefix + "_interdex_group_" + std::to_string(group_id),
                    group_size);
    TRACE(CLMG, 3, "InterDex Group %s_%u %zu", prefix.c_str(), group_id,
          group_size);
  }

  for (auto& pair : m_merging_size_counts) {
    auto merging_size = pair.first;
    auto count = pair.second;
    mgr.incr_metric(prefix + "_merging_size_" + std::to_string(merging_size),
                    count);
    TRACE(CLMG, 3, "Merging size %s_%zu %zu", prefix.c_str(), merging_size,
          count);
  }

  m_approx_stats.update_redex_stats(prefix, mgr);

  mgr.incr_metric(prefix + "_merger_class_generated", m_num_generated_classes);
  mgr.incr_metric(prefix + "_class_merged", m_num_classes_merged);
  mgr.incr_metric(prefix + "_ctor_dedupped", m_num_ctor_dedupped);
  mgr.incr_metric(prefix + "_static_non_virt_dedupped",
                  m_num_static_non_virt_dedupped);
  mgr.incr_metric(prefix + "_vmethods_dedupped", m_num_vmethods_dedupped);
  mgr.set_metric(prefix + "_const_lifted_methods", m_num_const_lifted_methods);
}

} // namespace class_merging
