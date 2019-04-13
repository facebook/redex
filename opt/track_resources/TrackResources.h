/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class TrackResourcesPass : public Pass {
 public:
  TrackResourcesPass() : Pass("TrackResourcesPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("classes_to_track", {}, m_classes_to_track);
    jw.get("tracked_fields_output", "", m_tracked_fields_output);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static void find_accessed_fields(
      Scope& fullscope,
      ConfigFiles& conf,
      std::unordered_set<DexClass*> classes_to_track,
      std::unordered_set<DexField*>& recorded_fields,
      std::unordered_set<std::string>& classes_to_search);

  static std::unordered_set<DexClass*> build_tracked_cls_set(
      const std::vector<std::string>& cls_suffixes,
      const ProguardMap& pg_map);

 private:
  std::vector<std::string> m_classes_to_track;
  std::string m_tracked_fields_output;
};
