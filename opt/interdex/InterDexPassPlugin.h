/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  // The InterDex pass might create additional classes, e.g. to hold
  // methods it relocates. Such classes get announced with this callback.
  // Note that such additional class are not allowed to affect virtual scopes of
  // classes in the original scope.
  virtual void add_to_scope(DexClass*) {}

  // Will prevent clazz from going into any output dex.
  virtual bool should_skip_class(const DexClass*) { return false; }

  // Whether the InterDex pass logic is allowed to move around methods
  // of a particular class.
  virtual bool should_not_relocate_methods_of_class(const DexClass*) {
    return false;
  }

  // Calculate the amount of refs that any classes from additional_classes
  // will add to the output dex (see below).
  virtual void gather_refs(const DexInfo&,
                           const DexClass*,
                           std::vector<DexMethodRef*>&,
                           std::vector<DexFieldRef*>&,
                           std::vector<DexType*>&,
                           std::vector<DexType*>&) {}

  // In each dex, reserve this many frefs to be potentially added after the
  // inter-dex pass
  virtual size_t reserve_frefs() { return 0; }

  // In each dex, reserve this many trefs to be potentially added after the
  // inter-dex pass
  virtual size_t reserve_trefs() { return 0; }

  // In each dex, reserve this many mrefs to be potentially added after the
  // inter-dex pass
  virtual size_t reserve_mrefs() { return 0; }

  // Return any new codegened classes that should be added to the current dex.
  virtual DexClasses additional_classes(const DexClassesVector&,
                                        const DexClasses&) {
    DexClasses empty;
    return empty;
  }

  // Return classes that should be added at the end. None, by default.
  virtual DexClasses leftover_classes() {
    DexClasses empty;
    return empty;
  }

  // Run plugin cleanup and finalization here. InterDex Pass should run
  // this after running its implementation
  virtual void cleanup(const std::vector<DexClass*>&) {}

  const std::string& name() const { return m_name; }

  virtual ~InterDexPassPlugin(){};

 private:
  void set_name(const std::string& new_name) { m_name = new_name; }

  std::string m_name;

  template <typename T>
  friend class ::PluginEntry;
};

using InterDexRegistry = PluginEntry<InterDexPassPlugin>;

} // namespace interdex
