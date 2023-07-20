/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexProperties.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ConfigFiles;
class DexStore;
using DexStoresVector = std::vector<DexStore>;
class PassManager;

namespace redex_properties {

class PropertyChecker;

class Manager {
 public:
  Manager(ConfigFiles& conf,
          std::vector<redex_properties::PropertyChecker*> checkers);

  bool property_is_enabled(const PropertyName& property_name) const;

  std::unordered_set<PropertyName> get_initial() const;
  static const std::unordered_set<PropertyName>& get_default_initial();

  std::unordered_set<PropertyName> get_final() const;
  static const std::unordered_set<PropertyName>& get_default_final();

  std::unordered_set<PropertyName> get_required(
      const PropertyInteractions& interactions) const;

  void check(DexStoresVector& stores, PassManager& mgr);

  const std::unordered_set<PropertyName>& apply(
      const PropertyInteractions& interactions);
  const std::unordered_set<PropertyName>& apply_and_check(
      const PropertyInteractions& interactions,
      DexStoresVector& stores,
      PassManager& mgr);

  static std::optional<std::string> verify_pass_interactions(
      const std::vector<std::pair<std::string, PropertyInteractions>>&
          pass_interactions,
      ConfigFiles& conf);

  const std::unordered_set<PropertyName>& get_established() const {
    return m_established;
  }

  static std::vector<PropertyName> get_all_properties();

  static bool is_negative(const PropertyName& property);

 private:
  // TODO: See whether we can make checkers use const.
  ConfigFiles& m_conf;

  std::unordered_set<PropertyName> m_established;

  std::vector<redex_properties::PropertyChecker*> m_checkers;
};

} // namespace redex_properties
