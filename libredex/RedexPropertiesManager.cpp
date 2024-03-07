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

const std::unordered_set<Property>& Manager::get_default_initial() {
  static const auto default_initial_properties = []() {
    std::unordered_set<Property> set;
#define REDEX_PROPS(name, _neg, is_init, _final, _def_pres) \
  if (is_init) {                                            \
    set.insert(Property::name);                             \
  }
#include "RedexProperties.def"
#undef REDEX_PROPS
    return set;
  }();
  return default_initial_properties;
}

std::unordered_set<Property> Manager::get_initial() const {
  return filter_out_disabled_properties(get_default_initial(), *this);
}

const std::unordered_set<Property>& Manager::get_default_final() {
  static const auto default_final_properties = []() {
    std::unordered_set<Property> set;
#define REDEX_PROPS(name, _neg, _init, is_final, _def_pres) \
  if (is_final) {                                           \
    set.insert(Property::name);                             \
  }
#include "RedexProperties.def"
#undef REDEX_PROPS
    return set;
  }();
  return default_final_properties;
}

std::unordered_set<Property> Manager::get_final() const {
  return filter_out_disabled_properties(get_default_final(), *this);
}

std::unordered_set<Property> Manager::get_required(
    const PropertyInteractions& interactions) const {
  std::unordered_set<Property> res;
  for (const auto& [property, interaction] : interactions) {
    if (interaction.requires_) {
      res.insert(property);
    }
  }
  return res;
}

void Manager::check(DexStoresVector& stores, PassManager& mgr) {
  for (auto* checker : m_checkers) {
    TRACE(PM, 3, "Checking for %s...", get_name(checker->get_property()));
    checker->run_checker(stores, m_conf, mgr,
                         m_established.count(checker->get_property()));
  }
}

const std::unordered_set<Property>& Manager::apply(
    const PropertyInteractions& interactions) {
  std20::erase_if(m_established, [&](const auto& property) {
    auto it = interactions.find(property);
    if (it == interactions.end()) {
      return !is_negative(property) && !is_default_preserving(property);
    }
    const auto& interaction = it->second;
    return !interaction.preserves;
  });
  for (const auto& [property, interaction] : interactions) {
    if (interaction.establishes) {
      m_established.insert(property);
    }
  }
  return m_established;
}

const std::unordered_set<Property>& Manager::apply_and_check(
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
    for (auto& property : m.m_established) {
      oss << property << ", ";
    }
    oss << "\n";
  };
  log_established_properties("initial state establishes");

  auto check = [&](const auto& properties) {
    for (auto& property : properties) {
      if (!m.m_established.count(property)) {
        oss << "    *** REQUIRED PROPERTY NOT CURRENTLY ESTABLISHED ***: "
            << property << "\n";
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
    for (const auto& [property, interaction] : interactions) {
      if (interaction.requires_finally) {
        final_properties.insert(property);
      }
    }
  }
  log_established_properties("final state requires");
  check(final_properties);

  for (auto& property : get_all_properties()) {
    if (!is_negative(property)) {
      continue;
    }
    if (m.m_established.count(property)) {
      oss << "    *** MUST-NOT PROPERTY IS ESTABLISHED IN FINAL STATE ***: "
          << property << "\n";
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

bool Manager::property_is_enabled(const Property& property) const {
  // If we had c++20, we could use a constexpr sorted std::array with
  // std::lower_bound for fast lookups...
  static const std::unordered_map<Property, bool (*)(const ConfigFiles&)>
      enable_check_funcs{{
          Property::HasSourceBlocks,
          [](const ConfigFiles& conf) {
            return pass_is_enabled("InsertSourceBlocksPass", conf);
          },
      }};

  auto it = enable_check_funcs.find(property);
  if (it == enable_check_funcs.end()) {
    return true;
  }

  return (it->second)(m_conf);
}

} // namespace redex_properties
