/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "PluginRegistry.h"

namespace interdex {

class InterDexPassPlugin {
 public:
  // Run plugin initialization here. InterDex pass should run this
  // before running its implementation.
  virtual void configure(const Scope&, ConfigFiles&) {}

  // Will prevent clazz from going into any output dex.
  virtual bool should_skip_class(const DexClass*) { return false; }

  // Calculate the amount of refs that any classes from additional_classes
  // will add to the output dex (see below).
  virtual void gather_refs(const DexClass*,
                           std::vector<DexMethodRef*>&,
                           std::vector<DexFieldRef*>&,
                           std::vector<DexType*>&,
                           std::vector<DexType*>&) {}

  // Return any new codegened classes that should be added to the current dex.
  virtual DexClasses additional_classes(size_t dex_count, const DexClasses&) {
    DexClasses empty;
    return empty;
  }

  // Run plugin cleanup and finalization here. InterDex Pass should run
  // this after running its implementation
  virtual void cleanup(const std::vector<DexClass*>&) {}

  const std::string& name() const { return m_name; }

  virtual ~InterDexPassPlugin() {}

 private:
  void set_name(const std::string& new_name) { m_name = new_name; }

  std::string m_name;

  template <typename T>
  friend class ::PluginEntry;
};

using InterDexRegistry = PluginEntry<InterDexPassPlugin>;

} // namespace interdex
