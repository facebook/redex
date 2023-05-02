/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexPropertiesManager.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string_view>

#include "ConfigFiles.h"
#include "RedexProperties.h"
#include "RedexPropertyChecker.h"
#include "StlUtil.h"
#include "Trace.h"

namespace redex_properties {

Manager::Manager(ConfigFiles& conf,
                 std::vector<redex_properties::PropertyChecker*> checkers)
    : m_conf(conf),
      m_established(get_initial()),
      m_checkers(std::move(checkers)) {
  // TODO: Filter m_checkers for unused properties.
}

namespace {

template <typename CollectionT>
CollectionT filter_out_disabled_properties(const CollectionT& c,
                                           const Manager& mgr) {
  CollectionT res;
  std::copy_if(c.begin(), c.end(), std::inserter(res, res.end()),
               [&mgr](const auto& x) { return mgr.property_is_enabled(x); });
  return res;
}

} // namespace

std::unordered_set<PropertyName> Manager::get_initial() const {
  using namespace names;
  static const std::unordered_set<PropertyName> default_initial_properties{
      UltralightCodePatterns};
  return filter_out_disabled_properties(default_initial_properties, *this);
}

std::unordered_set<PropertyName> Manager::get_final() const {
  using namespace names;
  static const std::unordered_set<PropertyName> default_final_properties{
      NoInitClassInstructions, DexLimitsObeyed};
  return filter_out_disabled_properties(default_final_properties, *this);
}

namespace {

using namespace names;

// TODO: Figure out a way to keep this complete.
//
//       We could move to an enum with the typical .def file to autogenerate.
std::vector<PropertyName> get_all_properties() {
  return {
      NoInitClassInstructions,
      DexLimitsObeyed,
      NeedsEverythingPublic,
      NeedsInjectionIdLowering,
      HasSourceBlocks,
      NoSpuriousGetClassCalls,
      RenameClass,
      UltralightCodePatterns,
  };
}

bool is_negative(const PropertyName& property) {
  return property == NeedsEverythingPublic ||
         property == NeedsInjectionIdLowering;
}

} // namespace

std::unordered_set<PropertyName> Manager::get_required(
    const PropertyInteractions& interactions) const {
  std::unordered_set<PropertyName> res;
  for (const auto& [property_name, interaction] : interactions) {
    if (interaction.requires_) {
      res.insert(property_name);
    }
  }
  return res;
}

void Manager::check(DexStoresVector& stores, PassManager& mgr) {
  for (auto* checker : m_checkers) {
    TRACE(PM, 3, "Checking for %s...", checker->get_property_name().c_str());
    checker->run_checker(stores, m_conf, mgr,
                         m_established.count(checker->get_property_name()));
  }
}

const std::unordered_set<PropertyName>& Manager::apply(
    const PropertyInteractions& interactions) {
  std20::erase_if(m_established, [&](const auto& property_name) {
    auto it = interactions.find(property_name);
    if (it == interactions.end()) {
      return !is_negative(property_name);
    }
    const auto& interaction = it->second;
    return !interaction.preserves;
  });
  for (const auto& [property_name, interaction] : interactions) {
    if (interaction.establishes) {
      m_established.insert(property_name);
    }
  }
  return m_established;
}

const std::unordered_set<PropertyName>& Manager::apply_and_check(
    const PropertyInteractions& interactions,
    DexStoresVector& stores,
    PassManager& mgr) {
  apply(interactions);
  check(stores, mgr);
  return m_established;
}

std::optional<std::string> Manager::verify_pass_interactions(
    const std::vector<std::pair<std::string, PropertyInteractions>>&
        pass_interactions,
    ConfigFiles& conf) {
  std::ostringstream oss;

  Manager m{conf, {}};

  bool failed{false};

  auto log_established_properties = [&](const std::string& title) {
    oss << "  " << title << ": ";
    for (auto& property_name : m.m_established) {
      oss << property_name << ", ";
    }
    oss << "\n";
  };
  log_established_properties("initial state establishes");

  auto check = [&](const auto& properties) {
    for (auto& property_name : properties) {
      if (!m.m_established.count(property_name)) {
        oss << "    *** REQUIRED PROPERTY NOT CURRENTLY ESTABLISHED ***: "
            << property_name << "\n";
        failed = true;
      }
    }
  };

  auto final_properties = m.get_final();

  for (auto& [pass_name, interactions] : pass_interactions) {
    auto required_properties = m.get_required(interactions);
    log_established_properties("requires");
    check(required_properties);
    oss << pass_name << "\n";
    m.apply(interactions);
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
    if (m.m_established.count(property_name)) {
      oss << "    *** MUST-NOT PROPERTY IS ESTABLISHED IN FINAL STATE ***: "
          << property_name << "\n";
      failed = true;
    }
  }

  if (failed) {
    return oss.str();
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

bool Manager::property_is_enabled(const PropertyName& property_name) const {
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

  return (it->second)(m_conf);
}

} // namespace redex_properties
