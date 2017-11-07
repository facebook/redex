/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class TrackResourcesPass : public Pass {
 public:
  TrackResourcesPass() : Pass("TrackResourcesPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("classes_to_track", {}, m_classes_to_track);
    pc.get("tracked_fields_output", "", m_tracked_fields_output);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static void find_accessed_fields(Scope& fullscope,
      ConfigFiles& cfg,
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
