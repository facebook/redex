/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once
#include "Trace.h"
#include "Util.h"
#include <functional>
#include <unordered_map>
#include <vector>

class Plugin {};

template <class T>
class PluginEntry : public Plugin {
 public:
  typedef std::function<T*()> Creator;
  void register_plugin(const std::string& plugin_name, Creator&& creator) {
    m_creators[plugin_name] = std::move(creator);
  }
  std::unique_ptr<T> create(const std::string& plugin_name) {
    if (m_creators.count(plugin_name)) {
      return std::unique_ptr<T>(m_creators[plugin_name]());
    }
    return nullptr;
  }
  std::vector<std::unique_ptr<T>> create_plugins() {
    std::vector<std::unique_ptr<T>> res;
    for (const auto& creator : m_creators) {
      res.emplace_back(std::unique_ptr<T>(creator.second()));
    }
    return res;
  }

 private:
  std::unordered_map<std::string, Creator> m_creators;
};

/**
 * Global registry of plugins. Passes should register their individual
 * PluginEntry during construction.
 * Other passes should register their plugins during the configure phase.
 * Finally during run_pass the pass should call into the PluginEntry to
 * instantiate all registered plugins (limited to 1 for now)
 */
class PluginRegistry {
 public:
  /**
   * Get the global registry object.
   */
  static PluginRegistry& get();

  void register_pass(const std::string& pass, std::unique_ptr<Plugin> plugin_entry);
  Plugin* pass_registry(const std::string& pass);

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  PluginRegistry() {}
  PluginRegistry(const PluginRegistry&) = delete;

  std::unordered_map<std::string, std::unique_ptr<Plugin>> m_registered_passes;
};
