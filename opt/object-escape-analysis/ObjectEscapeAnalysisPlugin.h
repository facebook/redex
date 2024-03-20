/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "PluginRegistry.h"

class ObjectEscapeAnalysisPlugin {
 public:
  virtual ~ObjectEscapeAnalysisPlugin() {}

  virtual void shrink_method(const init_classes::InitClassesWithSideEffects&,
                             DexMethod*) {}

  const std::string& name() const { return m_name; }

 private:
  void set_name(const std::string& new_name) { m_name = new_name; }

  std::string m_name;

  template <typename T>
  friend class ::PluginEntry;
};

static inline const char* OBJECTESCAPEANALYSIS_PASS_NAME =
    "ObjectEscapeAnalysisPass";

using ObjectEscapeAnalysisRegistry = PluginEntry<ObjectEscapeAnalysisPlugin>;
