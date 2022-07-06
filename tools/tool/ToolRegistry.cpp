/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ToolRegistry.h"
#include "Tool.h"

ToolRegistry& ToolRegistry::get() {
  static ToolRegistry registry;
  return registry;
}

void ToolRegistry::register_tool(Tool* tool) {
  m_registered_tools.push_back(tool);
}

const std::vector<Tool*>& ToolRegistry::get_tools() const {
  return m_registered_tools;
}

Tool* ToolRegistry::get_tool(const char* name) const {
  for (const auto tool : m_registered_tools) {
    if (!strcmp(name, tool->name().c_str())) {
      return tool;
    }
  }
  return nullptr;
}
