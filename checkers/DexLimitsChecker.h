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

class DexFieldRef;
class DexMethodRef;
class DexType;

namespace redex_properties {

class DexLimitsChecker : public PropertyChecker {
 public:
  DexLimitsChecker() : PropertyChecker(names::DexLimitsObeyed) {}

  void run_checker(DexStoresVector&, ConfigFiles&, PassManager&, bool) override;

  struct DexData {
    std::unordered_set<DexFieldRef*> fields;
    std::unordered_set<DexMethodRef*> methods;
    std::unordered_set<DexType*> types;
    std::unordered_set<DexType*> pending_init_class_fields;
    std::unordered_set<DexType*> pending_init_class_types;
  };

  std::unordered_map<std::string, std::vector<DexData>> m_data;
};

} // namespace redex_properties
