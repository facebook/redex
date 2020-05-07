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
      const std::unordered_map<const DexMethodRef*, method_profiles::Stats>&
          method_profile_stats);
  bool should_inline(
      const DexMethod* caller_method,
      const DexMethod* callee_method) const;

  bool enabled() const;

 private:
  const std::unordered_map<const DexMethodRef*, method_profiles::Stats>&
      m_method_profile_stats;
  std::unordered_set<const DexMethodRef*> m_hot_methods;
};
