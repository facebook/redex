/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string_view>

#include "ConfigFiles.h"
#include "RedexProperties.h"
#include "StlUtil.h"

namespace redex_properties {

template <typename CollectionT>
static CollectionT filter_out_disabled_properties(const CollectionT& c,
                                                  const ConfigFiles& conf) {
  CollectionT res;
  std::copy_if(c.begin(), c.end(), std::inserter(res, res.end()),
               [&conf](const auto& x) { return property_is_enabled(x, conf); });

  return res;
}

std::unordered_set<PropertyName> get_initial(const ConfigFiles& conf) {
  using namespace names;
  static const std::unordered_set<PropertyName> default_initial_properties{};
  return filter_out_disabled_properties(default_initial_properties, conf);
}

std::unordered_set<PropertyName> get_final(const ConfigFiles& conf) {
  using namespace names;
  static const std::unordered_set<PropertyName> default_final_properties{
      NoInitClassInstructions, DexLimitsObeyed};
  return filter_out_disabled_properties(default_final_properties, conf);
}

namespace {

using namespace names;

std::vector<PropertyName> get_all_properties() {
  return {
      NoInitClassInstructions,
      NeedsEverythingPublic,
  };
}

bool is_negative(const PropertyName& property) {
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
        pass_interactions,
    ConfigFiles& conf) {
  std::ostringstream oss;
  bool failed{false};
  auto established_properties = get_initial(conf);
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
  auto final_properties = get_final(conf);
  for (auto& [pass_name, interactions] : pass_interactions) {
    auto required_properties = get_required(interactions);
    log_established_properties("requires");
    check(required_properties);
    oss << pass_name << "\n";
    established_properties =
        redex_properties::apply(established_properties, interactions);
    log_established_properties("establishes");
    for (const auto& [property_name, interaction] : interactions) {
      if (interaction.requires_finally) {
        final_properties.insert(property_name);
      }
    }
  }
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

static bool pass_is_enabled(const std::string& pass_name,
                            const ConfigFiles& conf) {
  // If InsertSourceBlocksPass is not on the pass list, or is disabled, don't
  // run this check.
  const auto& json_config = conf.get_json_config();
  const auto& passes_from_config = json_config["redex"]["passes"];

  if (!std::any_of(passes_from_config.begin(), passes_from_config.end(),
                   [pass_name](const auto& pn) { return pn == pass_name; })) {
    return false;
  }

  if (json_config.contains(pass_name.c_str())) {
    const auto& pass_data = json_config[pass_name.c_str()];
    if (pass_data.isMember("disabled")) {
      if (pass_data["disabled"].asBool()) {
        return false;
      }
    }
  }

  return true;
}

bool property_is_enabled(const PropertyName& property_name,
                         const ConfigFiles& conf) {
  // If we had c++20, we could use a constexpr sorted std::array with
  // std::lower_bound for fast lookups...
  static const std::unordered_map<PropertyName, bool (*)(const ConfigFiles&)>
      enable_check_funcs{{
          names::HasSourceBlocks,
          [](const ConfigFiles& conf) {
            return pass_is_enabled("InsertSourceBlocksPass", conf);
          },
      }};

  auto it = enable_check_funcs.find(property_name);
  if (it == enable_check_funcs.end()) {
    return true;
  }

  return (it->second)(conf);
}

} // namespace redex_properties
