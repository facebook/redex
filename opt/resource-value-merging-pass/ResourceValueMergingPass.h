/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "Pass.h"
#include "RedexResources.h"

/**
 * The Resource Value Merging optimization pass analyzes dex code and resource
 * information to represent application logic in an equivalent, yet more
 * compact/efficient fashion for execution, specifically for Android UI
 * stylistic components.
 *
 * This optimization pass:
 * - Utilizes a graph representation of Android styles as nodes, and parent
 *   relationships as directed edges
 * - Analyzes resource data structures to find redundant pieces of information
 *   that can be combined, moved, and/or deleted
 * - Transforms the binary representation of Android resource metadata, defining
 *   APIs to perform serialization and manipulation in multiple Android
 *   container formats
 *
 * Config options:
 * - excluded_resources: A list of resources that should be excluded from the
 *   optimization
 */

using ResourceAttributeInformation = UnorderedSet<uint32_t>;
struct OptimizableResources {
  // resource_id -> { attribute_ids }
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;
  // resource_id -> (attribute_id -> value)
  UnorderedMap<uint32_t,
               UnorderedMap<uint32_t, resources::StyleResource::Value>>
      additions;
};

class ResourceValueMergingPass : public Pass {
 public:
  ResourceValueMergingPass() : Pass("ResourceValueMergingPass") {}

  void bind_config() override {
    bind("excluded_resources", {}, m_excluded_resources);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  ResourceAttributeInformation get_resource_attributes(
      uint32_t resource_id, const resources::StyleMap& style_map);

  OptimizableResources get_resource_optimization(
      const resources::StyleInfo& style_info,
      const UnorderedSet<uint32_t>& ambiguous_styles,
      const UnorderedSet<uint32_t>& directly_reachable_styles);

  std::optional<resources::StyleResource::Value>
  get_common_attribute_among_children(
      const UnorderedSet<uint32_t>& resource_ids,
      uint32_t attribute_id,
      const resources::StyleMap& style_map);

  OptimizableResources remove_unoptimizable_resources(
      const OptimizableResources& optimizable_candidates,
      const UnorderedSet<uint32_t>& directly_reachable_styles);

  std::optional<const resources::StyleResource> find_style_resource(
      uint32_t resource_id, const resources::StyleMap& style_map);

  resources::StyleInfo get_optimized_graph(
      const resources::StyleInfo& initial,
      const UnorderedSet<uint32_t>& ambiguous_styles,
      const UnorderedSet<uint32_t>& directly_reachable_styles);

  void apply_additions_to_style_graph(
      resources::StyleInfo& style_info,
      const UnorderedMap<
          uint32_t,
          UnorderedMap<uint32_t, resources::StyleResource::Value>>& additions);

  void apply_removals_to_style_graph(
      resources::StyleInfo& style_info,
      UnorderedMap<uint32_t, ResourceAttributeInformation> removals);

  OptimizableResources get_graph_diffs(
      const resources::StyleInfo& inital,
      const resources::StyleInfo& optimized,
      const UnorderedSet<uint32_t>& ambiguous_styles);

  void remove_attribute_from_descendent(
      uint32_t resource_id,
      const UnorderedMap<uint32_t, resources::StyleResource::Value>& attr_map,
      const resources::StyleInfo& optimized,
      UnorderedMap<uint32_t, ResourceAttributeInformation>& removals);

  std::vector<std::vector<uint32_t>> get_resources_to_merge(
      const resources::StyleInfo& style_info,
      const UnorderedSet<uint32_t>& ambiguous_styles,
      const UnorderedSet<uint32_t>& directly_reachable_styles);

  resources::StyleModificationSpec::Modification
  get_parent_and_attribute_modifications_for_merging(
      const resources::StyleInfo& style_info,
      const std::vector<uint32_t>& resources_to_merge);

 private:
  ResourceAttributeInformation find_resource_optimization_candidates(
      resources::StyleInfo::vertex_t vertex,
      const resources::StyleInfo& style_info,
      OptimizableResources& optimizable_candidates,
      const UnorderedSet<uint32_t>& ambiguous_styles);

  void find_resources_to_merge(
      resources::StyleInfo::vertex_t vertex,
      const resources::StyleInfo& style_info,
      const UnorderedSet<uint32_t>& ambiguous_styles,
      const UnorderedSet<uint32_t>& directly_reachable_styles,
      std::vector<std::vector<uint32_t>>& resources_to_merge);

  std::vector<resources::StyleModificationSpec::Modification>
  get_style_merging_modifications(
      const resources::StyleInfo& style_info,
      const std::vector<std::vector<uint32_t>>& resources_to_merge);

  UnorderedSet<std::string> m_excluded_resources;
};
