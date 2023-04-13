/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sstream>

#include "RedexProperties.h"
#include "StlUtil.h"

namespace redex_properties {

std::unordered_set<PropertyName> get_initial() {
  using namespace names;
  return {};
}

std::unordered_set<PropertyName> get_final() {
  using namespace names;
  return {NoInitClassInstructions};
}

namespace {

using namespace names;

std::vector<PropertyName> get_all_properties() {
  return {
      NoInitClassInstructions,
      NeedsEverythingPublic,
  };
}

bool is_negative(PropertyName property) {
  return property == NeedsEverythingPublic;
}

} // namespace

std::unordered_set<PropertyName> get_required(
    const PropertyInteractions& interactions) {
  std::unordered_set<PropertyName> res;
  for (const auto& [property_name, interaction] : interactions) {
    if (interaction.requires_) {
      res.insert(property_name);
    }
  }
  return res;
}
std::unordered_set<PropertyName> apply(
    std::unordered_set<PropertyName> established_properties,
    const PropertyInteractions& interactions) {
  std20::erase_if(established_properties, [&](const auto& property_name) {
    auto it = interactions.find(property_name);
    if (it == interactions.end()) {
      return !is_negative(property_name);
    }
    const auto& interaction = it->second;
    return !interaction.preserves;
  });
  for (const auto& [property_name, interaction] : interactions) {
    if (interaction.establishes) {
      established_properties.insert(property_name);
    }
  }
  return established_properties;
}

std::optional<std::string> verify_pass_interactions(
    const std::vector<std::pair<std::string, PropertyInteractions>>&
        pass_interactions) {
  std::ostringstream oss;
  bool failed{false};
  auto established_properties = get_initial();
  auto log_established_properties = [&](const std::string& title) {
    oss << "  " << title << ": ";
    for (auto& property_name : established_properties) {
      oss << property_name << ", ";
    }
    oss << "\n";
  };
  log_established_properties("initial state establishes");
  auto check = [&](const auto& properties) {
    for (auto& property_name : properties) {
      if (!established_properties.count(property_name)) {
        oss << "    *** REQUIRED PROPERTY NOT CURRENTLY ESTABLISHED ***: "
            << property_name << "\n";
        failed = true;
      }
    }
  };
  for (auto& [pass_name, interactions] : pass_interactions) {
    auto required_properties = get_required(interactions);
    log_established_properties("requires");
    check(required_properties);
    oss << pass_name << "\n";
    established_properties =
        redex_properties::apply(established_properties, interactions);
    log_established_properties("establishes");
  }
  auto final_properties = get_final();
  log_established_properties("final state requires");
  check(final_properties);

  for (auto& property_name : get_all_properties()) {
    if (!is_negative(property_name)) {
      continue;
    }
    if (established_properties.count(property_name)) {
      oss << "    *** MUST-NOT PROPERTY IS ESTABLISHED IN FINAL STATE ***: "
          << property_name << "\n";
      failed = true;
    }
  }

  if (failed) {
    return std::optional<std::string>(oss.str());
  }

  return std::nullopt;
}

} // namespace redex_properties
