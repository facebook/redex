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
    UnorderedSet<DexFieldRef*> fields;
    UnorderedSet<DexMethodRef*> methods;
    UnorderedSet<DexType*> types;
    UnorderedSet<DexType*> pending_init_class_fields;
    UnorderedSet<DexType*> pending_init_class_types;
  };

  UnorderedMap<std::string, std::vector<DexData>> m_data;
};

} // namespace redex_properties
