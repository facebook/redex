/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourceValueMergingPass.h"
#include "ConfigFiles.h"
#include "DeterministicContainers.h"
#include "Pass.h"
#include "PassManager.h"
#include "RedexResources.h"
#include "Styles.h"
#include "Trace.h"

#define MAX_ITERATIONS 20

using namespace resources;
void print_resources(const UnorderedMap<uint32_t, ResourceAttributeInformation>&
                         optimized_resources) {
  if (!traceEnabled(RES, 3)) {
    return;
  }
  TRACE(RES, 3, "Resources that can be optimized via deletion:");
  for (const auto& [resource_id, attr_ids] :
       UnorderedIterable(optimized_resources)) {
    std::string attributes = "Attribute ID: ";
    for (const auto& attr_id : UnorderedIterable(attr_ids)) {
      std::ostringstream oss;
      oss << " 0x" << std::hex << attr_id << " ";
      attributes += oss.str();
    }
    TRACE(RES, 3, "Resource ID: 0x%x; %s", resource_id, attributes.c_str());
  }
}

void print_resources(
    const UnorderedMap<uint32_t,
                       UnorderedMap<uint32_t, resources::StyleResource::Value>>&
        optimized_resources) {
  if (!traceEnabled(RES, 3)) {
    return;
  }
  TRACE(RES, 3, "Resources that can be optimized via merging:");
  for (const auto& [resource_id, attr_map] :
       UnorderedIterable(optimized_resources)) {
    std::string attributes = "Attributes: ";
    for (const auto& [attr_id, _] : UnorderedIterable(attr_map)) {
      std::ostringstream oss;
      oss << " 0x" << std::hex << attr_id << " ";
      attributes += oss.str();
    }
    TRACE(RES, 3, "Resource ID: 0x%x; %s", resource_id, attributes.c_str());
  }
}

void ResourceValueMergingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  TRACE(RES, 1, "ResourceValueMergingPass excluded_resources count: %zu",
        m_excluded_resources.size());

  for (const auto& resource : UnorderedIterable(m_excluded_resources)) {
    TRACE(RES, 1, "  Excluded resource: %s", resource.c_str());
  }

  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);

  auto resources = create_resource_reader(apk_dir);
  auto res_table = resources->load_res_table();
  auto resource_files = resources->find_resources_files();
  auto style_info = res_table->load_style_info();

  resources::ReachabilityOptions options;
  StyleAnalysis style_analysis(apk_dir, conf.get_global_config(), options,
                               stores, UnorderedSet<uint32_t>());

  const auto& ambiguous_styles = style_analysis.ambiguous_styles();
  const auto& directly_reachable_styles =
      style_analysis.directly_reachable_styles();
  const auto& optimized_resources = get_resource_optimization(
      style_info, ambiguous_styles, directly_reachable_styles);
  print_resources(optimized_resources.removals);

  std::vector<resources::StyleModificationSpec::Modification> modifications;
  for (const auto& [resource_id, attributes] :
       UnorderedIterable(optimized_resources.removals)) {
    for (const auto& attribute_id : UnorderedIterable(attributes)) {
      modifications.push_back(resources::StyleModificationSpec::Modification(
          resource_id, attribute_id));
    }
  }
  res_table->apply_attribute_removals(modifications, resource_files);
}

/**
 * Helper method to find a style resource for a given resource ID.
 * Returns nullopt if the resource doesn't exist or has multiple style
 * resources.
 */
std::optional<const resources::StyleResource>
ResourceValueMergingPass::find_style_resource(
    uint32_t resource_id, const resources::StyleMap& style_map) {
  auto style_it = style_map.find(resource_id);
  if (style_it == style_map.end()) {
    return std::nullopt;
  }

  const auto& style_resources = style_it->second;
  if (style_resources.size() != 1) {
    return std::nullopt;
  }

  return style_resources[0];
}

std::optional<resources::StyleResource::Value>
ResourceValueMergingPass::get_common_attribute_among_children(
    const UnorderedSet<uint32_t>& resource_ids,
    uint32_t attribute_id,
    const resources::StyleMap& style_map) {

  if (resource_ids.empty()) {
    return std::nullopt;
  }

  std::vector<resources::StyleResource::Value> values;
  for (const auto& resource_id : UnorderedIterable(resource_ids)) {
    const auto& style_resource_opt =
        find_style_resource(resource_id, style_map);
    if (!style_resource_opt.has_value()) {
      return std::nullopt;
    }

    const auto& style_resource = style_resource_opt.value();
    auto attr_it = style_resource.attributes.find(attribute_id);
    if (attr_it == style_resource.attributes.end()) {
      return std::nullopt;
    }

    values.push_back(attr_it->second);
  }

  if (values.empty()) {
    return std::nullopt;
  }

  resources::StyleResource::Value common_value = values[0];
  for (size_t value_idx = 1; value_idx < values.size(); value_idx++) {
    if (!(common_value == values[value_idx])) {
      return std::nullopt;
    }
  }

  return common_value;
}

OptimizableResources ResourceValueMergingPass::get_resource_optimization(
    const resources::StyleInfo& style_info,
    const UnorderedSet<uint32_t>& ambiguous_styles,
    const UnorderedSet<uint32_t>& directly_reachable_styles) {
  OptimizableResources optimizable_candidates;
  auto root_vertices = style_info.get_roots();

  for (const auto& vertex : UnorderedIterable(root_vertices)) {
    find_resource_optimization_candidates(
        vertex, style_info, optimizable_candidates, ambiguous_styles);
  }
  return remove_unoptimizable_resources(optimizable_candidates,
                                        directly_reachable_styles);
}

OptimizableResources ResourceValueMergingPass::remove_unoptimizable_resources(
    const OptimizableResources& optimizable_candidates,
    const UnorderedSet<uint32_t>& directly_reachable_styles) {
  OptimizableResources optimizable_resources;

  for (const auto& [resource_id, attr_ids] :
       UnorderedIterable(optimizable_candidates.removals)) {
    if (directly_reachable_styles.find(resource_id) ==
        directly_reachable_styles.end()) {
      optimizable_resources.removals[resource_id] = attr_ids;
    }
  }

  for (const auto& [resource_id, attr_map] :
       UnorderedIterable(optimizable_candidates.additions)) {
    if (directly_reachable_styles.find(resource_id) ==
        directly_reachable_styles.end()) {
      optimizable_resources.additions[resource_id] = attr_map;
    }
  }

  return optimizable_resources;
}

ResourceAttributeInformation get_common_attributes(
    const std::vector<ResourceAttributeInformation>& attributes) {
  if (attributes.empty()) {
    return ResourceAttributeInformation();
  }

  ResourceAttributeInformation common_attributes = attributes[0];

  for (size_t attribute_idx = 1; attribute_idx < attributes.size();
       attribute_idx++) {
    ResourceAttributeInformation intersection;
    for (const auto& item : UnorderedIterable(attributes[attribute_idx])) {
      if (common_attributes.find(item) != common_attributes.end()) {
        intersection.insert(item);
      }
    }
    common_attributes = intersection;
  }
  return common_attributes;
}

/**
 * Used to track and store common attributes that exist
 * across all style definitions for the same resource ID.
 * These resources will only process unambiguous styles.
 */
ResourceAttributeInformation ResourceValueMergingPass::get_resource_attributes(
    uint32_t resource_id, const resources::StyleMap& style_map) {
  ResourceAttributeInformation attributes;

  const auto& style_resource_opt = find_style_resource(resource_id, style_map);
  if (!style_resource_opt.has_value()) {
    return attributes;
  }

  const auto& style_resource = style_resource_opt.value();
  for (const auto& attr_pair : style_resource.attributes) {
    attributes.insert(attr_pair.first);
  }

  return attributes;
}

/**
 * Resources are eligible for optimization if they meet specific criteria:
 *
 * For deletion candidates:
 * - Attributes that are common across a parent style and all of its children
 * - These attributes can be safely deleted from the parent since they're
 * already defined in all children
 *
 * For hoisted candidates:
 * - Attributes that are common across all children of a parent style
 * - These attributes have identical values in all children
 * - These can be "pulled up" and hoisted into the parent style
 *
 * The optimization process analyzes the style hierarchy graph:
 * - Starting from root styles (those with no incoming edges)
 * - Traversing down to identify common attributes
 * - Marking attributes for either deletion or hoisted based on value equality
 *
 * @return Returns the set of attribute IDs that are defined in the current
 * style resource. This is used by parent nodes to determine which attributes
 * are common across the style hierarchy.
 */
ResourceAttributeInformation
ResourceValueMergingPass::find_resource_optimization_candidates(
    resources::StyleInfo::vertex_t vertex,
    const resources::StyleInfo& style_info,
    OptimizableResources& optimizable_candidates,
    const UnorderedSet<uint32_t>& ambiguous_styles) {
  uint32_t resource_id = style_info.graph[vertex].id;

  if (ambiguous_styles.find(resource_id) != ambiguous_styles.end()) {
    return ResourceAttributeInformation();
  }

  ResourceAttributeInformation resources_common_attributes =
      get_resource_attributes(resource_id, style_info.styles);

  auto children_count = boost::out_degree(vertex, style_info.graph);
  if (!children_count) {
    return resources_common_attributes;
  }

  std::vector<ResourceAttributeInformation> child_attributes;
  UnorderedSet<uint32_t> children_resource_ids;

  for (const auto& edge :
       boost::make_iterator_range(boost::out_edges(vertex, style_info.graph))) {
    auto target_vertex = boost::target(edge, style_info.graph);
    const auto& child_attr = find_resource_optimization_candidates(
        target_vertex, style_info, optimizable_candidates, ambiguous_styles);
    child_attributes.push_back(child_attr);
    children_resource_ids.insert(style_info.graph[target_vertex].id);
  }

  const auto& common_child_attributes = get_common_attributes(child_attributes);
  ResourceAttributeInformation optimized_attributes;

  // These attributes are common across all children and their parent (current
  // node), and the attribute in the parent can be deleted.
  for (const auto& attr_id : UnorderedIterable(resources_common_attributes)) {
    if (common_child_attributes.find(attr_id) !=
        common_child_attributes.end()) {
      optimizable_candidates.removals[resource_id].insert(attr_id);
      optimized_attributes.insert(attr_id);
    }
  }

  // These are attributes that are common across all children and that can
  // potentially be hoisted into their parent.
  for (const auto& attr_id : UnorderedIterable(common_child_attributes)) {
    const auto& common_value = get_common_attribute_among_children(
        children_resource_ids, attr_id, style_info.styles);
    if (optimized_attributes.find(attr_id) == optimized_attributes.end() &&
        common_value.has_value()) {
      optimizable_candidates.additions[resource_id].insert(
          {attr_id, common_value.value()});
    }
  }

  return resources_common_attributes;
}

void ResourceValueMergingPass::remove_attribute_from_descendent(
    uint32_t resource_id,
    const UnorderedMap<uint32_t, resources::StyleResource::Value>& attr_map,
    const resources::StyleInfo& optimized,
    UnorderedMap<uint32_t, ResourceAttributeInformation>& removals) {
  const auto& vertex_find = optimized.id_to_vertex.find(resource_id);
  always_assert(vertex_find != optimized.id_to_vertex.end());
  auto vertex = vertex_find->second;

  for (const auto& edge :
       boost::make_iterator_range(boost::out_edges(vertex, optimized.graph))) {
    auto child_vertex = boost::target(edge, optimized.graph);
    uint32_t child_resource_id = optimized.graph[child_vertex].id;
    for (const auto& [attr_id, _] : UnorderedIterable(attr_map)) {
      removals[child_resource_id].insert(attr_id);
    }
  }
}

resources::StyleInfo ResourceValueMergingPass::get_optimized_graph(
    const resources::StyleInfo& initial,
    const UnorderedSet<uint32_t>& ambiguous_styles,
    const UnorderedSet<uint32_t>& directly_reachable_styles) {
  resources::StyleInfo optimized(initial);
  int iteration = 0;

  auto [removals, additions] = get_resource_optimization(
      initial, ambiguous_styles, directly_reachable_styles);

  while ((!removals.empty() || !additions.empty()) &&
         iteration < MAX_ITERATIONS) {

    // apply modifications to in-memory style graph
    iteration++;
  }

  return optimized;
}

void ResourceValueMergingPass::apply_additions_to_style_graph(
    resources::StyleInfo& style_info,
    const UnorderedMap<uint32_t,
                       UnorderedMap<uint32_t, resources::StyleResource::Value>>&
        additions) {
  for (const auto& [resource_id, attr_map] : UnorderedIterable(additions)) {
    auto style_it = style_info.styles.find(resource_id);
    always_assert(style_it != style_info.styles.end());

    auto& style_resources = style_it->second;

    // There should only be one style resource per resource ID based on the fact
    // that optimizations only occur on unambiguous styles.
    always_assert(style_resources.size() == 1);

    auto& style_resource = style_resources[0];
    for (const auto& [attr_id, value] : UnorderedIterable(attr_map)) {
      style_resource.attributes.insert({attr_id, value});
    }
  }
}

void ResourceValueMergingPass::apply_removals_to_style_graph(
    resources::StyleInfo& style_info,
    UnorderedMap<uint32_t, ResourceAttributeInformation> removals) {
  for (const auto& [resource_id, attr_ids] : UnorderedIterable(removals)) {

    auto style_it = style_info.styles.find(resource_id);
    always_assert(style_it != style_info.styles.end());

    auto& style_resources = style_it->second;

    always_assert(style_resources.size() == 1);

    auto& style_resource = style_resources[0];

    for (const auto& attr_id : UnorderedIterable(attr_ids)) {
      auto attr_it = style_resource.attributes.find(attr_id);
      always_assert(attr_it != style_resource.attributes.end());
      style_resource.attributes.erase(attr_it);
    }
  }
}

// Implements a set difference (A - B) = {x | x ∈ A and x ∉ B}
UnorderedMap<uint32_t, resources::StyleResource::Value>
find_attribute_differences(const StyleResource& A, const StyleResource& B) {
  UnorderedMap<uint32_t, resources::StyleResource::Value> diff_attrs;

  // x ∈ A
  for (const auto& [attr_id, value] : UnorderedIterable(A.attributes)) {
    // x ∈ B
    bool is_in_B = B.attributes.find(attr_id) != B.attributes.end();

    if (!is_in_B) {
      diff_attrs.insert({attr_id, value});
    }
  }

  return diff_attrs;
}

OptimizableResources ResourceValueMergingPass::get_graph_diffs(
    const resources::StyleInfo& inital,
    const resources::StyleInfo& optimized,
    const UnorderedSet<uint32_t>& ambiguous_styles) {
  OptimizableResources diff;

  // No resources should have been removed in previous iterations, so the
  // resources in the initial and optimized graph should be the same
  UnorderedSet<uint32_t> all_resource_ids;
  for (const auto& [resource_id, _] : UnorderedIterable(inital.styles)) {
    if (ambiguous_styles.find(resource_id) != ambiguous_styles.end()) {
      continue;
    }
    all_resource_ids.insert(resource_id);
  }

  for (const auto& resource_id : UnorderedIterable(all_resource_ids)) {
    auto initial_style_it = inital.styles.find(resource_id);
    auto optimized_style_it = optimized.styles.find(resource_id);

    always_assert_log(initial_style_it != inital.styles.end() &&
                          optimized_style_it != optimized.styles.end(),
                      "Resource 0x%x not found", resource_id);

    const StyleResource& initial_style = initial_style_it->second[0];
    const StyleResource& optimized_style = optimized_style_it->second[0];

    auto removal_attrs =
        find_attribute_differences(initial_style, optimized_style);
    for (const auto& [attr_id, _] : UnorderedIterable(removal_attrs)) {
      diff.removals[resource_id].insert(attr_id);
    }

    auto addition_attrs =
        find_attribute_differences(optimized_style, initial_style);
    for (const auto& [attr_id, value] : UnorderedIterable(addition_attrs)) {
      diff.additions[resource_id].insert({attr_id, value});
    }

    for (const auto& [attr_id, initial_value] :
         UnorderedIterable(initial_style.attributes)) {
      auto optimized_attr_it = optimized_style.attributes.find(attr_id);
      if (optimized_attr_it == optimized_style.attributes.end()) {
        continue;
      }
      const auto& optimized_value = optimized_attr_it->second;
      if (!(initial_value == optimized_value)) {
        diff.removals[resource_id].insert(attr_id);
        diff.additions[resource_id].insert({attr_id, optimized_value});
      }
    }
  }

  return diff;
}

static ResourceValueMergingPass s_pass;
