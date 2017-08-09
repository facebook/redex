/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
