/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>

#include "DeterministicContainers.h"
#include "GlobalConfig.h"
#include "ReachableResources.h"
#include "RedexResources.h"

class StyleAnalysis {
 public:
  StyleAnalysis(const std::string& zip_dir,
                const GlobalConfig& global_config,
                const resources::ReachabilityOptions& options,
                DexStoresVector& stores,
                const UnorderedSet<uint32_t>& additional_roots)
      : StyleAnalysis(
            zip_dir,
            *global_config.get_config_by_name<ResourceConfig>("resources"),
            options,
            stores,
            additional_roots) {}
  StyleAnalysis(const std::string& zip_dir,
                const ResourceConfig& global_resources_config,
                resources::ReachabilityOptions options,
                DexStoresVector& stores,
                const UnorderedSet<uint32_t>& additional_roots)
      : m_options(std::move(options)), m_roots(additional_roots) {
    m_options.granular_style_reachability = true;
    m_reachable_resources = std::make_unique<resources::ReachableResources>(
        zip_dir, global_resources_config, m_options);
    auto code_roots = m_reachable_resources->get_resource_roots(stores);
    for (auto root : UnorderedIterable(code_roots)) {
      m_roots.insert(root);
    }
    auto* res_table = m_reachable_resources->get_res_table();
    m_style_info = res_table->load_style_info();
  }

  UnorderedSet<uint32_t> directly_reachable_styles();
  UnorderedSet<uint32_t> ambiguous_styles();
  std::string dot(bool exclude_nodes_with_no_edges = false,
                  bool display_attributes = false);

 private:
  resources::ReachabilityOptions m_options;
  UnorderedSet<uint32_t> m_roots;
  std::unique_ptr<resources::ReachableResources> m_reachable_resources;
  resources::StyleInfo m_style_info;
  std::optional<UnorderedSet<uint32_t>> m_directly_reachable_styles;
};
