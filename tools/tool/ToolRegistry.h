/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

class Tool;

/**
 * Global registry of tools.  Construction of a tool automatically registers
 * it here.  Tool should be constructed statically before main.
 */
struct ToolRegistry {
  /**
   * Get the global registry object.
   */
  static ToolRegistry& get();

  /**
   * Register a tool.
   */
  void register_tool(Tool* tool);

  /**
   * Get the tools.
   */
  const std::vector<Tool*>& get_tools() const;

  /**
   * Get a tool by name.
   */
  Tool* get_tool(const char* name) const;

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  ToolRegistry() {}
  ToolRegistry(const ToolRegistry&) = delete;

  std::vector<Tool*> m_registered_tools;
};
