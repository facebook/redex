/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

struct DevirtualizerConfigs {
  bool vmethods_not_using_this = true;
  bool vmethods_using_this = false;
  bool dmethods_not_using_this = true;
  bool dmethods_using_this = false;
  bool ignore_keep = false;

  DevirtualizerConfigs(bool vmethods_not_using_this = true,
                       bool vmethods_using_this = false,
                       bool dmethods_not_using_this = true,
                       bool dmethods_using_this = false,
                       bool ignore_keep = false)
      : vmethods_not_using_this(vmethods_not_using_this),
        vmethods_using_this(vmethods_using_this),
        dmethods_not_using_this(dmethods_not_using_this),
        dmethods_using_this(dmethods_using_this),
        ignore_keep(ignore_keep) {}
};

struct DevirtualizerMetrics {
  uint32_t num_methods_not_using_this{0};
  uint32_t num_methods_using_this{0};
  uint32_t num_virtual_calls{0};
  uint32_t num_direct_calls{0};
  uint32_t num_super_calls{0};
};

class MethodDevirtualizer {
 public:
  explicit MethodDevirtualizer(DevirtualizerConfigs& config)
      : m_config(config) {}
  MethodDevirtualizer(bool vmethods_not_using_this,
                      bool vmethods_using_this,
                      bool dmethods_not_using_this,
                      bool dmethods_using_this,
                      bool ignore_keep) {
    m_config = {vmethods_not_using_this,
                vmethods_using_this,
                dmethods_not_using_this,
                dmethods_using_this,
                ignore_keep};
  }

  DevirtualizerMetrics devirtualize_methods(const Scope& scope) {
    return devirtualize_methods(scope, scope);
  }

  DevirtualizerMetrics devirtualize_methods(
      const Scope& scope, const std::vector<DexClass*>& target_classes);

  // Assuming vmethods.
  DevirtualizerMetrics devirtualize_vmethods(
      const Scope& scope, const std::vector<DexMethod*>& methods);

 private:
  DevirtualizerConfigs m_config;
  DevirtualizerMetrics m_metrics;

  void reset_metrics() { m_metrics = DevirtualizerMetrics(); }

  void staticize_methods_using_this(
      const std::vector<DexClass*>& scope,
      const std::unordered_set<DexMethod*>& methods);

  void staticize_methods_not_using_this(
      const std::vector<DexClass*>& scope,
      const std::unordered_set<DexMethod*>& methods);

  void verify_and_split(const std::vector<DexMethod*>& candidates,
                        std::unordered_set<DexMethod*>& using_this,
                        std::unordered_set<DexMethod*>& not_using_this);
};
