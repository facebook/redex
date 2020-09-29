/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "Trace.h"
#include "Util.h"
#include <functional>
#include <unordered_map>
#include <vector>

struct Plugin {
  virtual ~Plugin() = default;
};

template <class T>
class PluginEntry : public Plugin {
 public:
  using Creator = std::function<T*()>;
  void register_plugin(const std::string& plugin_name, Creator&& creator) {
    if (m_creators.count(plugin_name) != 0) {
      // TODO: Make this an error once all existing configurations have been
      // cleaned up.
      fprintf(stderr,
              "[plugins] Warning: A plug-in of this name has already been "
              "registered :: %s\n",
              plugin_name.c_str());
      m_ordered_creator_names.erase(std::find(m_ordered_creator_names.begin(),
                                              m_ordered_creator_names.end(),
                                              plugin_name));
    }
    m_creators[plugin_name] = std::move(creator);
    m_ordered_creator_names.push_back(plugin_name);
  }
  std::unique_ptr<T> create(const std::string& plugin_name) {
    if (m_creators.count(plugin_name)) {
      auto plugin = std::unique_ptr<T>(m_creators[plugin_name]());
      if (plugin) {
        plugin->set_name(plugin_name);
      }
      return plugin;
    }
    return nullptr;
  }
  std::vector<std::unique_ptr<T>> create_plugins() {
    std::vector<std::unique_ptr<T>> res;
    for (const auto& name : m_ordered_creator_names) {
      res.emplace_back(std::move(create(name)));
    }
    return res;
  }

 private:
  std::unordered_map<std::string, Creator> m_creators;
  std::vector<std::string> m_ordered_creator_names;
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

  void register_pass(const std::string& pass,
                     std::unique_ptr<Plugin> plugin_entry);
  Plugin* pass_registry(const std::string& pass);

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  PluginRegistry() {}
  PluginRegistry(const PluginRegistry&) = delete;

  std::unordered_map<std::string, std::unique_ptr<Plugin>> m_registered_passes;
};
