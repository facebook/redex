/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexPropertyChecker.h"

#include <string>
#include <vector>

#include "DeterministicContainers.h"

class DexFieldRef;
class DexMethodRef;
class DexType;

namespace redex_properties {

class DexLimitsChecker : public PropertyChecker {
 public:
  DexLimitsChecker() : PropertyChecker(names::DexLimitsObeyed) {}

  void run_checker(DexStoresVector&, ConfigFiles&, PassManager&, bool) override;

  struct DexData {
    // These should be sets, but for overhead reasons we keep these as vectors
    // and only translate them when we find an issue.
    std::vector<DexFieldRef*> fields;
    std::vector<DexMethodRef*> methods;
    std::vector<const DexType*> types;
    // These are hopefully small and transitioning is annoying.
    UnorderedSet<const DexType*> pending_init_class_fields;
    UnorderedSet<const DexType*> pending_init_class_types;
  };

  UnorderedMap<std::string, std::vector<DexData>> m_data;
};

} // namespace redex_properties
