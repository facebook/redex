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
#include <vector>

#include "DeterministicContainers.h"

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

  bool property_is_enabled(const Property& property) const;

  UnorderedSet<Property> get_initial() const;
  static const UnorderedSet<Property>& get_default_initial();

  UnorderedSet<Property> get_final() const;
  static const UnorderedSet<Property>& get_default_final();

  UnorderedSet<Property> get_required(
      const PropertyInteractions& interactions) const;

  void check(DexStoresVector& stores, PassManager& mgr);

  const UnorderedSet<Property>& apply(const PropertyInteractions& interactions);
  const UnorderedSet<Property>& apply_and_check(
      const PropertyInteractions& interactions,
      DexStoresVector& stores,
      PassManager& mgr);

  static std::optional<std::string> verify_pass_interactions(
      const std::vector<std::pair<std::string, PropertyInteractions>>&
          pass_interactions,
      ConfigFiles& conf);

  const UnorderedSet<Property>& get_established() const {
    return m_established;
  }

 private:
  // TODO: See whether we can make checkers use const.
  ConfigFiles& m_conf;

  UnorderedSet<Property> m_established;

  std::vector<redex_properties::PropertyChecker*> m_checkers;
};

} // namespace redex_properties
