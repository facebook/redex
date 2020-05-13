/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "MethodProfiles.h"

class InlineForSpeed final {
 public:
  explicit InlineForSpeed(
      const method_profiles::MethodProfiles* method_profiles);
  bool should_inline(const DexMethod* caller_method,
                     const DexMethod* callee_method) const;

  bool enabled() const;

 private:
  void compute_hot_methods();
  bool should_inline_per_interaction(
      const DexMethod* caller_method,
      const DexMethod* callee_method,
      uint32_t caller_insns,
      uint32_t callee_insns,
      const std::string& interaction_id,
      const method_profiles::StatsMap& method_stats) const;

  const method_profiles::MethodProfiles* m_method_profiles;
  std::map<std::string, std::pair<double, double>> m_min_scores;
};
