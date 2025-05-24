/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourcesValidationHelper.h"

#include <gtest/gtest.h>

#include "DeterministicContainers.h"

void validate_walk_references_for_resource(ResourceTableFile* res_table) {
  auto get_id = [&](const std::string& name) {
    return res_table->name_to_ids.at(name).at(0);
  };
  auto id = get_id("CustomText.Prickly");
  auto parent_id = get_id("CustomText");
  // Common validation
  auto validate_common = [&](const UnorderedSet<uint32_t>& nodes_visited,
                             const UnorderedSet<std::string>& file_paths) {
    EXPECT_EQ(nodes_visited.count(get_id("prickly_green")), 1)
        << "Should return ID for prickly_green";
    EXPECT_EQ(nodes_visited.count(get_id("welcome_text_size")), 1)
        << "Should return ID for welcome_text_size";
    EXPECT_TRUE(file_paths.empty());
  };

  // Default options.
  {
    resources::ReachabilityOptions options_default;
    UnorderedSet<uint32_t> nodes_visited;
    UnorderedSet<std::string> file_paths;
    res_table->walk_references_for_resource(id,
                                            ResourcePathType::ZipPath,
                                            options_default,
                                            &nodes_visited,
                                            &file_paths);
    EXPECT_EQ(nodes_visited.count(parent_id), 1)
        << "Expected to visit parent ref";
    validate_common(nodes_visited, file_paths);
  }
  // Enable granular reach option.
  {
    resources::ReachabilityOptions options_granular;
    options_granular.granular_style_reachability = true;
    UnorderedSet<uint32_t> nodes_visited;
    UnorderedSet<std::string> file_paths;
    res_table->walk_references_for_resource(id,
                                            ResourcePathType::ZipPath,
                                            options_granular,
                                            &nodes_visited,
                                            &file_paths);
    EXPECT_EQ(nodes_visited.count(parent_id), 0)
        << "Should not visit parent ref";
    validate_common(nodes_visited, file_paths);
  }
}
