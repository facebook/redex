/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct ProguardMap;

class TrackResourcesPass : public Pass {
 public:
  TrackResourcesPass() : Pass("TrackResourcesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{DexLimitsObeyed, {.preserves = true}},
            {HasSourceBlocks, {.preserves = true}},
            {NoSpuriousGetClassCalls, {.preserves = true}}};
  }

  void bind_config() override {
    bind("classes_to_track", {}, m_classes_to_track);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static size_t find_accessed_fields(
      const Scope& fullscope,
      const std::unordered_set<DexClass*>& classes_to_track,
      const std::unordered_set<std::string>& classes_to_search,
      std::unordered_set<DexField*>* recorded_fields);

  static std::unordered_set<DexClass*> build_tracked_cls_set(
      const std::vector<std::string>& cls_suffixes, const ProguardMap& pg_map);

 private:
  std::vector<std::string> m_classes_to_track;
};
