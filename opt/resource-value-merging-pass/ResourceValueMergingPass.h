/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "Pass.h"

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

class ResourceValueMergingPass : public Pass {
 public:
  ResourceValueMergingPass() : Pass("ResourceValueMergingPass") {}

  void bind_config() override {
    bind("excluded_resources", {}, m_excluded_resources);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  UnorderedSet<std::string> m_excluded_resources;
};
